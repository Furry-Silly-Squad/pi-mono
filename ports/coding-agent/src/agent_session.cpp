#include "agent_session.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>
#include <sstream>
#include <thread>

#include "compaction.hpp"
#include "context_loader.hpp"
#include "file_ops.hpp"
#include "gpu_semaphore.hpp"
#include "session_entry.hpp"
#include "subagent.hpp"
#include "system_prompt.hpp"
#include "tools/bash_destructive.hpp"
#include "tools/tool.hpp"

namespace coding_agent {
namespace {

std::string read_file_contents(const std::string& path) {
    std::ifstream input(path);
    std::stringstream buffer;
    buffer << input.rdbuf();
    return buffer.str();
}

std::vector<std::string> parse_tool_name_list(const std::string& csv) {
    std::vector<std::string> result;
    std::istringstream ss(csv);
    std::string token;
    while (std::getline(ss, token, ',')) {
        const auto first = token.find_first_not_of(" \t");
        if (first == std::string::npos) continue;
        const auto last = token.find_last_not_of(" \t");
        result.push_back(token.substr(first, last - first + 1));
    }
    return result;
}

std::string describe_tool_call(const std::string& tool_name, const std::string& args_json) {
    try {
        const auto args = nlohmann::json::parse(args_json);
        if (tool_name == "edit") {
            return "edit " + args.value("path", "");
        }
        if (tool_name == "bash") {
            std::string cmd = args.value("command", "");
            if (cmd.size() > 80) cmd = cmd.substr(0, 80) + "...";
            return "bash: " + cmd;
        }
        if (tool_name == "read") {
            return "read " + args.value("path", "");
        }
        if (tool_name == "write") {
            return "write " + args.value("path", "");
        }
        if (tool_name == "grep") {
            std::string path = args.value("path", "");
            if (path.empty() && args.contains("paths")) {
                path = args.at("paths").get<std::string>();
            }
            return "grep '" + args.value("pattern", "") + "' " + path;
        }
        if (tool_name == "find") {
            const std::string name = args.value("name", "");
            return "find " + args.value("path", "") + (name.empty() ? "" : " -name " + name);
        }
        if (tool_name == "ls") {
            return "ls " + args.value("path", "");
        }
    } catch (...) {}
    return tool_name;
}

bool is_whitespace_or_empty_content(const std::string& s) {
    for (unsigned char c : s) {
        if (!std::isspace(c)) return false;
    }
    return true;
}

// User-visible follow-up when the model stops with no tools and no message body (common with some servers/models).
const char* kEmptyCompletionUserNudge =
    "Your last reply had no message text. Briefly summarize what you did, what you found, and "
    "what the user should do next. If more tool calls are required, use them. Do not reply with an "
    "empty message.";

/// Match `approx_tokens` (4 chars per token): show tail of the last N token-equivalents.
constexpr int kDebugTailApproxTokens = 20;

std::string escape_debug_text(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (unsigned char c : s) {
        if (c == '\n') {
            out += "\\n";
        } else if (c == '\r') {
            out += "\\r";
        } else if (c == '\t') {
            out += "\\t";
        } else {
            out += static_cast<char>(c);
        }
    }
    return out;
}

std::string assistant_tail_for_debug(const std::string& content) {
    const int max_chars = std::max(1, kDebugTailApproxTokens * 4);
    if (content.size() <= static_cast<size_t>(max_chars)) {
        return escape_debug_text(content);
    }
    return std::string("...") +
           escape_debug_text(content.substr(content.size() - static_cast<size_t>(max_chars)));
}

}  // namespace

// ============================================================================
// ThinkingLevel helpers
// ============================================================================

const char* thinking_level_to_string(ThinkingLevel level) {
    switch (level) {
        case ThinkingLevel::Off:     return "off";
        case ThinkingLevel::Minimal: return "minimal";
        case ThinkingLevel::Low:     return "low";
        case ThinkingLevel::Medium:  return "medium";
        case ThinkingLevel::High:    return "high";
        case ThinkingLevel::XHigh:   return "xhigh";
    }
    return "off";
}

ThinkingLevel string_to_thinking_level(const std::string& str) {
    if (str == "minimal") return ThinkingLevel::Minimal;
    if (str == "low")     return ThinkingLevel::Low;
    if (str == "medium")  return ThinkingLevel::Medium;
    if (str == "high")    return ThinkingLevel::High;
    if (str == "xhigh")   return ThinkingLevel::XHigh;
    return ThinkingLevel::Off;
}

// ============================================================================
// Constructor / Destructor
// ============================================================================

AgentSession::AgentSession(const AgentSessionConfig& config,
                           Provider& provider,
                           ToolRegistry& tools,
                           std::unique_ptr<SessionManager> session)
    : config_(config),
      provider_(provider),
      tools_(tools),
      session_(std::move(session)),
      current_model_(config.model),
      current_thinking_level_(ThinkingLevel::Off) {

    active_tool_names_ = parse_tool_name_list(config_.initial_active_tools);

    loadSessionContextIntoAgent();
}

AgentSession::~AgentSession() = default;

void AgentSession::set_destructive_bash_confirm(std::function<bool(const std::string& command)> fn) {
    destructive_bash_confirm_ = std::move(fn);
}

// ============================================================================
// Run Loop
// ============================================================================

bool AgentSession::run(const std::string& user_input,
                       const ChunkCallback& on_chunk,
                       std::atomic<bool>* cancel_flag) {
    if (running_.exchange(true)) return false;
    abort_requested_.store(false);

    struct Guard {
        std::atomic<bool>& flag;
        ~Guard() { flag.store(false); }
    } guard{running_};

    return run_turn(user_input, on_chunk, cancel_flag);
}

void AgentSession::abort() {
    abort_requested_.store(true);
    provider_.cancel();
}

bool AgentSession::is_running() const { return running_.load(); }

// ============================================================================
// State Access
// ============================================================================

const std::string&             AgentSession::model()          const { return current_model_; }
ThinkingLevel                  AgentSession::thinking_level() const { return current_thinking_level_; }
const std::string&             AgentSession::system_prompt()  const { return current_system_prompt_; }
const std::vector<ChatMessage>& AgentSession::messages()      const { return messages_; }
size_t                         AgentSession::message_count()  const { return messages_.size(); }
const AgentSessionConfig&      AgentSession::session_config() const { return config_; }

std::vector<std::string> AgentSession::active_tools() const {
    return active_tool_names_;
}

// ============================================================================
// Model Management
// ============================================================================

bool AgentSession::set_model(const std::string& model_id) {
    if (model_id == current_model_) return false;
    const std::string prev = current_model_;
    current_model_   = model_id;
    config_.model    = model_id;
    session_->appendModelChange(config_.provider, model_id);
    AgentEvent ev{};
    ev.type           = AgentEvent::Type::ModelChange;
    ev.previous_model = prev;
    ev.new_model      = model_id;
    emit_event(ev.type, ev);
    return true;
}

bool AgentSession::cycle_model(bool /*forward*/) {
    // Deferred: no model list available in Phase 7.
    return false;
}

void AgentSession::set_thinking_level(ThinkingLevel level) {
    if (level == current_thinking_level_) return;
    const ThinkingLevel prev = current_thinking_level_;
    current_thinking_level_ = level;
    session_->appendThinkingLevelChange(thinking_level_to_string(level));
    AgentEvent ev{};
    ev.type                    = AgentEvent::Type::ThinkingLevelChange;
    ev.previous_thinking_level = prev;
    ev.new_thinking_level      = level;
    emit_event(ev.type, ev);
}

bool AgentSession::cycle_thinking_level(bool forward) {
    const auto levels = available_thinking_levels();
    auto it = std::find(levels.begin(), levels.end(), current_thinking_level_);
    if (it == levels.end()) {
        set_thinking_level(levels.front());
        return true;
    }
    if (forward) {
        ++it;
        if (it == levels.end()) it = levels.begin();
    } else {
        if (it == levels.begin()) it = levels.end();
        --it;
    }
    if (*it == current_thinking_level_) return false;
    set_thinking_level(*it);
    return true;
}

std::vector<ThinkingLevel> AgentSession::available_thinking_levels() const {
    return {
        ThinkingLevel::Off,
        ThinkingLevel::Minimal,
        ThinkingLevel::Low,
        ThinkingLevel::Medium,
        ThinkingLevel::High,
        ThinkingLevel::XHigh,
    };
}

// ============================================================================
// Tool Management
// ============================================================================

void AgentSession::set_active_tools(const std::vector<std::string>& tool_names) {
    active_tool_names_ = tool_names;
}

std::vector<ToolDefinition> AgentSession::all_tool_definitions() const {
    return tools_.build_tool_definitions();
}

// ============================================================================
// Compaction (manual)
// ============================================================================

bool AgentSession::compact() {
    if (compacting_.exchange(true)) return false;
    struct Guard {
        std::atomic<bool>& flag;
        ~Guard() { flag.store(false); }
    } guard{compacting_};

    AgentEvent start_ev{};
    start_ev.type = AgentEvent::Type::CompactionStart;
    emit_event(start_ev.type, start_ev);

    std::string error;
    CompactionStats stats;
    const bool ok = compact_history(
        messages_,
        provider_,
        current_model_,
        config_.compaction_keep_recent_tokens,
        config_.compaction_reserve_tokens,
        &last_compaction_file_ops_,
        &stats,
        error
    );

    AgentEvent end_ev{};
    end_ev.type              = AgentEvent::Type::CompactionEnd;
    end_ev.compaction_success = ok && stats.did_compact;

    if (ok && stats.did_compact) {
        end_ev.tokens_before      = stats.tokens_before;
        end_ev.tokens_after       = stats.tokens_after;
        end_ev.compaction_summary = stats.summary;
        last_compaction_stats_    = stats;
        compaction_count_++;

        nlohmann::json details{
            {"read_files", sorted_file_list(stats.file_ops.read_files)},
            {"modified_files", sorted_file_list(stats.file_ops.modified_files)},
            {"tokens_after", stats.tokens_after},
        };
        session_->appendCompaction(stats.summary, stats.first_kept_entry_id, stats.tokens_before,
                                  std::make_optional(details));
        last_compaction_file_ops_.read_files.clear();
        last_compaction_file_ops_.modified_files.clear();
        for (const auto& p : stats.file_ops.read_files) {
            last_compaction_file_ops_.read_files.insert(p);
        }
        for (const auto& p : stats.file_ops.modified_files) {
            last_compaction_file_ops_.modified_files.insert(p);
        }
    } else if (!ok) {
        end_ev.compaction_error = error;
    }

    emit_event(end_ev.type, end_ev);
    return ok && stats.did_compact;
}

void AgentSession::abort_compaction() {
    // Not separately controllable in Phase 7.
}

bool AgentSession::is_compacting() const { return compacting_.load(); }

const CompactionStats& AgentSession::last_compaction_stats() const {
    return last_compaction_stats_;
}

// ============================================================================
// Event Handling
// ============================================================================

void AgentSession::set_event_handler(AgentEventHandler handler) {
    event_handler_ = std::move(handler);
}

void AgentSession::loadSessionContextIntoAgent() {
    SessionContext ctx = session_->buildSessionContext();
    messages_ = std::move(ctx.messages);
    if (!ctx.model.modelId.empty()) {
        current_model_   = ctx.model.modelId;
        config_.model    = ctx.model.modelId;
    }
    if (!ctx.thinkingLevel.empty()) {
        current_thinking_level_ = string_to_thinking_level(ctx.thinkingLevel);
    }

    if (messages_.empty() || messages_.front().role != "system") {
        std::vector<ContextFile> context_files;
        if (!config_.no_context_files) {
            context_files = load_context_files(config_.cwd);
        }
        current_system_prompt_ = build_system_prompt(
            config_.cwd,
            config_.no_tools
                ? std::vector<ToolDefinition>{}
                : tools_.build_tool_definitions(),
            context_files,
            config_.append_system_prompts
        );
        if (config_.system_prompt_path.has_value()) {
            current_system_prompt_ = read_file_contents(config_.system_prompt_path.value());
        }
        ChatMessage sys{
            .role    = "system",
            .content = current_system_prompt_,
        };
        messages_.insert(messages_.begin(), sys);
        session_->appendMessage(sys);
    } else {
        current_system_prompt_ = messages_.front().content;
    }
}

// ============================================================================
// Session Switching
// ============================================================================

void AgentSession::switchSession(std::unique_ptr<SessionManager> new_session) {
    session_               = std::move(new_session);
    compaction_count_    = 0;
    last_compaction_stats_ = {};
    turn_index_          = 0;
    loadSessionContextIntoAgent();
}

// ============================================================================
// Branching
// ============================================================================

void AgentSession::branch() {
    (void)branchWithSummary("Branch", std::nullopt);
}

void AgentSession::branchFrom(const std::string& branchFromId) {
    if (!session_->getEntry(branchFromId).has_value()) {
        std::cerr << "Error: entry not found: " << branchFromId << "\n";
        return;
    }
    session_->branch(branchFromId);
    loadSessionContextIntoAgent();
    std::cout << "Active branch moved to entry " << branchFromId << "\n";
}

std::string AgentSession::branchWithSummary(const std::string& summary,
                                             const std::optional<std::string>& branchFromId) {
    std::string resultId;
    if (branchFromId.has_value()) {
        if (!session_->getEntry(branchFromId.value()).has_value()) {
            std::cerr << "Error: entry not found: " << branchFromId.value() << "\n";
            return "";
        }
        resultId = session_->branchWithSummary(branchFromId, summary);
    } else {
        auto leafId = session_->getLeafId();
        if (!leafId.has_value()) {
            std::cerr << "Error: no leaf entry to branch from\n";
            return "";
        }
        resultId =
            session_->branchWithSummary(std::make_optional(leafId.value()), summary);
    }
    loadSessionContextIntoAgent();
    std::cout << "Branch created with summary, entry: " << resultId << "\n";
    return resultId;
}

// ============================================================================
// New Session
// ============================================================================

std::string AgentSession::createNewSession() {
    auto newSessionMgr = SessionManager::create(config_.cwd, "");
    if (!newSessionMgr) {
        std::cerr << "Error: failed to create new session\n";
        return "";
    }
    switchSession(std::move(newSessionMgr));
    std::cout << "New session created: " << session_id() << "\n";
    return session_id();
}

// ============================================================================
// Session Info
// ============================================================================

std::string AgentSession::session_id()   const { return session_->getSessionId(); }
std::string AgentSession::session_path() const {
    const auto p = session_->getSessionFile();
    return p.has_value() ? *p : "";
}
int         AgentSession::compaction_count()      const { return compaction_count_; }
int         AgentSession::total_context_tokens()  const {
    return ::coding_agent::total_context_tokens(messages_);
}

const TurnDebugInfo& AgentSession::last_turn_debug() const { return last_turn_debug_; }

void AgentSession::finalize_turn_debug(int model_rounds,
                                       bool hit_max_tool_iterations,
                                       const std::string& failure_kind,
                                       const std::string& provider_err,
                                       int empty_completion_nudges) {
    last_turn_debug_                 = TurnDebugInfo{};
    last_turn_debug_.model_rounds    = model_rounds;
    last_turn_debug_.hit_max_tool_iterations = hit_max_tool_iterations;
    last_turn_debug_.run_failure_kind      = failure_kind;
    last_turn_debug_.provider_error        = provider_err;
    last_turn_debug_.empty_completion_nudges = empty_completion_nudges;

    for (auto it = messages_.rbegin(); it != messages_.rend(); ++it) {
        if (it->role == "assistant") {
            last_turn_debug_.final_assistant_content_chars =
                static_cast<int>(it->content.size());
            last_turn_debug_.final_assistant_tool_call_count = it->tool_calls.size();
            last_turn_debug_.final_assistant_tail_esc        = assistant_tail_for_debug(it->content);
            break;
        }
    }
    if (!messages_.empty()) {
        last_turn_debug_.trailing_message_role = messages_.back().role;
    }
}

// ============================================================================
// Internal: run_turn
// ============================================================================

bool AgentSession::run_turn(const std::string& user_input,
                            const ChunkCallback& on_chunk,
                            std::atomic<bool>* cancel_flag) {
    ChatMessage user{
        .role    = "user",
        .content = user_input,
    };
    user.entry_id = session_->appendMessage(user);
    messages_.push_back(user);

    // Phase 11: Attempt decomposition before normal processing
    // If this looks like a multi-task request, try to decompose and execute subtasks
    if (decomposeAndExecute(user_input, on_chunk, cancel_flag)) {
        // Subtasks were executed and results injected. Continue to let the main agent
        // respond to those results (the turn loop below will pick up from here).
        // We don't break here because we want the main agent to see and respond to results.
    }

    // Deliver pending next-turn messages (asides) alongside the user prompt.
    for (const auto& aside : pending_next_turn_messages_) {
        ChatMessage aside_msg{
            .role    = "user",
            .content = aside,
        };
        aside_msg.entry_id = session_->appendMessage(aside_msg);
        messages_.push_back(aside_msg);
    }
    pending_next_turn_messages_.clear();

    AgentEvent ts_ev{};
    ts_ev.type        = AgentEvent::Type::TurnStart;
    ts_ev.turn_index  = turn_index_;
    emit_event(ts_ev.type, ts_ev);

    int model_rounds               = 0;
    bool hit_max_tool_iterations   = false;
    int empty_completion_nudges    = 0;

    for (int iter = 0; iter < config_.max_tool_iterations; ++iter) {
        // Check for abort before each model call.
        if (abort_requested_.load(std::memory_order_acquire)) {
            emit_abort_event();
            finalize_turn_debug(model_rounds, false, "interrupted", "", empty_completion_nudges);
            return false;
        }

        // Signal before each model call (drives between-tool animation resume).
        AgentEvent mc_ev{};
        mc_ev.type = AgentEvent::Type::ModelCallStart;
        emit_event(mc_ev.type, mc_ev);

        // Drain steering queue before each LLM call.
        auto steering_msgs = steering_queue_.drain();
        for (const auto& [text, images] : steering_msgs) {
            (void)images;
            ChatMessage steer_msg{
                .role    = "user",
                .content = text,
            };
            steer_msg.entry_id = session_->appendMessage(steer_msg);
            messages_.push_back(steer_msg);
        }
        emit_queue_update();

        ChatResponse response;
        std::string error;

        // Apply transformContext if configured (trim or augment messages before each provider call).
        std::vector<ChatMessage> provider_messages = messages_;
        if (config_.transform_context) {
            provider_messages = config_.transform_context(messages_);
        }

        if (!call_provider(provider_messages, active_tool_definitions(), response, error, on_chunk, cancel_flag)) {
            if (error == "interrupted") {
                // abort() sets abort_requested_ then provider.cancel(); interruption may surface
                // here rather than at the top-of-loop check, so emit Abort in that case too.
                if (abort_requested_.load(std::memory_order_acquire)) {
                    emit_abort_event();
                }
                if (!messages_.empty() && messages_.back().role == "user") {
                    messages_.pop_back();
                }
                finalize_turn_debug(model_rounds, false, "interrupted", "", empty_completion_nudges);
                return false;
            }
            AgentEvent err_ev{};
            err_ev.type          = AgentEvent::Type::Error;
            err_ev.error_message = error;
            emit_event(err_ev.type, err_ev);
            finalize_turn_debug(model_rounds, false, "error", error, empty_completion_nudges);

            // Check for retryable error
            if (handle_retryable_error(on_chunk, cancel_flag)) {
                return true;  // Retry initiated, skip compaction
            }
            return false;
        }

        ++model_rounds;

        ChatMessage assistant{
            .role         = "assistant",
            .content      = response.content,
            .tool_calls   = response.tool_calls,
            .usage_tokens = response.completion_tokens,
        };
        assistant.entry_id = session_->appendMessage(assistant);
        messages_.push_back(assistant);

        // Per-turn token breakdown.
        const int content_tok = response_content_tokens(response);
        const int tool_tok    = response_tool_calls_tokens(response);
        std::ostringstream tok_line;
        tok_line << "→ " << (content_tok + tool_tok)
                 << " tokens (content: " << content_tok
                 << ", tool_calls: "     << tool_tok << ")";
        if (!response.tool_calls.empty()) {
            tok_line << "\n  tools: ";
            for (size_t i = 0; i < response.tool_calls.size(); ++i) {
                if (i > 0) tok_line << ", ";
                tok_line << response.tool_calls[i].name;
            }
            tok_line << "\n";
        }
        on_chunk(tok_line.str());

        check_and_compact(on_chunk);

        if (response.tool_calls.empty()) {
            if (!is_whitespace_or_empty_content(response.content)) {
                // Agent stopped with content. Check follow-up queue before ending.
                auto followup_msgs = follow_up_queue_.drain();
                if (!followup_msgs.empty()) {
                    for (const auto& [text, images] : followup_msgs) {
                        (void)images;
                        ChatMessage fu_msg{
                            .role    = "user",
                            .content = text,
                        };
                        fu_msg.entry_id = session_->appendMessage(fu_msg);
                        messages_.push_back(fu_msg);
                    }
                    emit_queue_update();
                    on_chunk("\n[queued follow-up message; continuing turn…]\n");
                    continue;  // Continue the turn with follow-up messages
                }
                break;  // No follow-up, turn ends
            }
            // Empty content: nudge or check follow-up
            if (config_.max_empty_completion_nudges > 0 &&
                empty_completion_nudges < config_.max_empty_completion_nudges) {
                ++empty_completion_nudges;
                ChatMessage nudge{
                    .role    = "user",
                    .content = kEmptyCompletionUserNudge,
                };
                nudge.entry_id = session_->appendMessage(nudge);
                messages_.push_back(nudge);
                on_chunk("\n[empty assistant message; retrying with nudge…]\n");
                continue;
            }

            // Agent would stop. Check follow-up queue.
            auto followup_msgs = follow_up_queue_.drain();
            if (!followup_msgs.empty()) {
                for (const auto& [text, images] : followup_msgs) {
                    (void)images;
                    ChatMessage fu_msg{
                        .role    = "user",
                        .content = text,
                    };
                    fu_msg.entry_id = session_->appendMessage(fu_msg);
                    messages_.push_back(fu_msg);
                }
                emit_queue_update();
                on_chunk("\n[queued follow-up message; continuing turn…]\n");
                continue;  // Continue the turn with follow-up messages
            }
            break;  // No more messages, turn ends
        }

        execute_tools(response.tool_calls, on_chunk);
        if (iter == config_.max_tool_iterations - 1) {
            hit_max_tool_iterations = true;
        }
    }

    ++turn_index_;
    AgentEvent te_ev{};
    te_ev.type       = AgentEvent::Type::TurnEnd;
    te_ev.turn_index = turn_index_ - 1;
    emit_event(te_ev.type, te_ev);

    // Reset retry state on successful completion.
    if (retry_attempt_ > 0) {
        AgentEvent end_ev{};
        end_ev.type = AgentEvent::Type::AutoRetryEnd;
        end_ev.retry_success = true;
        end_ev.retry_attempt = retry_attempt_;
        emit_event(end_ev.type, end_ev);
        on_chunk("[retry] Success after " + std::to_string(retry_attempt_) + " attempts\n");
        retry_attempt_ = 0;
        retry_deadline_.reset();
        resolve_retry();
    }

    finalize_turn_debug(model_rounds, hit_max_tool_iterations, "", "", empty_completion_nudges);
    return true;
}

// ============================================================================
// Internal: call_provider
// ============================================================================

bool AgentSession::call_provider(const std::vector<ChatMessage>& history,
                                 const std::vector<ToolDefinition>& tool_defs,
                                 ChatResponse& response,
                                 std::string& error,
                                 const ChunkCallback& on_chunk,
                                 std::atomic<bool>* cancel_flag) {
    constexpr int k_tool_round_min_tokens = 8192;
    int max_tokens = config_.max_tokens;
    if (!tool_defs.empty()) {
        max_tokens = std::max(max_tokens, k_tool_round_min_tokens);
    }

    const ChatRequest request{
        .messages    = history,
        .tools       = tool_defs,
        .model       = current_model_,
        .max_tokens  = max_tokens,
        .temperature = config_.temperature,
        .stream      = config_.stream,
    };

    return provider_.chat(request, response, on_chunk, error, cancel_flag);
}

// ============================================================================
// Internal: execute_tools
// ============================================================================

// Execute a single tool and return the result (no event emission).
// Used by both sequential and parallel execution paths.
ToolResult AgentSession::execute_single_tool_raw(const ToolCall& call,
                                                  const ChunkCallback& on_chunk) {
    (void)on_chunk;  // on_chunk is used for display in the caller
    ToolResult result{.ok = false, .content = {}};
    bool run_dispatch = true;
    if (call.name == "bash") {
        try {
            const auto args = nlohmann::json::parse(call.arguments_json);
            const std::string command = args.at("command").get<std::string>();
            if (bash_command_looks_destructive(command)) {
                if (!destructive_bash_confirm_) {
                    run_dispatch = false;
                    result = {.ok = false,
                              .content = "Dangerous command blocked (no interactive confirmation "
                                         "handler); use interactive mode to confirm destructive commands."};
                } else if (!destructive_bash_confirm_(command)) {
                    run_dispatch = false;
                    result = {.ok = false,
                              .content = "Blocked destructive command: user did not allow execution"};
                }
            }
        } catch (const std::exception& ex) {
            run_dispatch = false;
            result = {.ok = false, .content = ex.what()};
        }
    }
    if (run_dispatch) {
        result = tools_.dispatch(call.name, call.arguments_json, config_.cwd);
    }
    return result;
}

// Execute a single tool with hooks, returning the final result.
// Before hooks can block execution; after hooks can modify the result.
ToolResult AgentSession::execute_single_tool_with_hooks(const ToolCall& call) {
    ToolResult result{.ok = false, .content = {}};

    // Run beforeToolCall hook (sequential preflight)
    if (config_.before_tool_call) {
        BeforeToolCallContext ctx{
            .tool_name = call.name,
            .tool_call_id = call.id,
            .args = call.arguments_json,
        };
        BeforeToolCallResult hook_result = config_.before_tool_call(ctx);
        if (hook_result.block) {
            result = {.ok = false, .content = hook_result.reason.empty()
                                                  ? "Tool execution was blocked"
                                                  : hook_result.reason};
            return result;
        }
    }

    // Execute the tool
    result = execute_single_tool_raw(call, [](const std::string&) {});

    // Run afterToolCall hook
    if (config_.after_tool_call && result.ok) {
        AfterToolCallContext ctx{
            .tool_name = call.name,
            .tool_call_id = call.id,
            .args = call.arguments_json,
            .result = result.content,
            .isError = !result.ok,
        };
        AfterToolCallResult hook_result = config_.after_tool_call(ctx);
        if (!hook_result.content.empty()) {
            result.content = hook_result.content;
        }
    }

    return result;
}

// Emit tool result event and append tool message (thread-safe via mutex).
void AgentSession::emit_tool_result(const ToolCall& call,
                                    const ToolResult& result) {
    std::lock_guard<std::mutex> lock(tool_dispatch_mutex_);

    AgentEvent res_ev{};
    res_ev.type         = AgentEvent::Type::ToolResult;
    res_ev.tool_name    = call.name;
    res_ev.tool_call_id = call.id;
    res_ev.tool_result  = result.content;
    res_ev.tool_error   = !result.ok;
    emit_event(res_ev.type, res_ev);

    ChatMessage tool_msg{
        .role         = "tool",
        .content      = (result.ok ? "ok" : "error") + std::string(": ") + result.content,
        .tool_call_id = call.id,
    };
    tool_msg.entry_id = session_->appendMessage(tool_msg);
    messages_.push_back(tool_msg);
}

// Execute a single tool, emitting events and appending messages (sequential path).
void AgentSession::execute_single_tool(const ToolCall& call,
                                       const ChunkCallback& on_chunk) {
    AgentEvent call_ev{};
    call_ev.type         = AgentEvent::Type::ToolCall;
    call_ev.tool_name    = call.name;
    call_ev.tool_call_id = call.id;
    call_ev.tool_args    = call.arguments_json;
    emit_event(call_ev.type, call_ev);

    on_chunk("[tool: " + call.name + "] " + describe_tool_call(call.name, call.arguments_json) + "\n");

    ToolResult result = execute_single_tool_with_hooks(call);

    on_chunk("[tool: " + call.name + "] " +
             (result.ok ? "done" : "failed: " + result.content) + "\n");

    emit_tool_result(call, result);
}

bool AgentSession::execute_tools(const std::vector<ToolCall>& tool_calls,
                                 const ChunkCallback& on_chunk) {
    if (tool_calls.empty()) return true;

    const bool global_parallel = config_.tool_execution_mode == "parallel";

    // Check if any tool in the batch is marked sequential.
    // If global mode is parallel but any tool is sequential, run the entire batch sequentially.
    bool batch_is_sequential = !global_parallel;
    if (global_parallel && !batch_is_sequential) {
        const auto all_defs = tools_.build_tool_definitions();
        for (const auto& call : tool_calls) {
            for (const auto& def : all_defs) {
                if (def.name == call.name &&
                    def.execution_mode == ToolExecutionMode::Sequential) {
                    batch_is_sequential = true;
                    break;
                }
            }
            if (batch_is_sequential) break;
        }
    }

    if (batch_is_sequential || tool_calls.size() == 1) {
        // Sequential execution (default, any sequential tool present, or single tool)
        for (const auto& call : tool_calls) {
            execute_single_tool(call, on_chunk);
        }
        return true;
    }

    // Parallel execution: emit ToolCall events and UI lines in assistant order (main thread),
    // run dispatches concurrently, then emit tool results and chunks in the same order.
    struct ToolWorkItem {
        ToolCall call;
        ToolResult result;
    };

    std::vector<ToolWorkItem> work_items;
    work_items.reserve(tool_calls.size());
    for (const auto& call : tool_calls) {
        work_items.push_back(ToolWorkItem{.call = call, .result = {.ok = false, .content = ""}});
    }

    for (const auto& item : work_items) {
        AgentEvent call_ev{};
        call_ev.type         = AgentEvent::Type::ToolCall;
        call_ev.tool_name    = item.call.name;
        call_ev.tool_call_id = item.call.id;
        call_ev.tool_args    = item.call.arguments_json;
        emit_event(call_ev.type, call_ev);
        on_chunk("[tool: " + item.call.name + "] " +
                 describe_tool_call(item.call.name, item.call.arguments_json) + "\n");
    }

    // Run beforeToolCall hooks sequentially (preflight) for all tools.
    for (size_t i = 0; i < tool_calls.size(); ++i) {
        auto& item = work_items[i];
        if (config_.before_tool_call) {
            BeforeToolCallContext ctx{
                .tool_name = item.call.name,
                .tool_call_id = item.call.id,
                .args = item.call.arguments_json,
            };
            BeforeToolCallResult hook_result = config_.before_tool_call(ctx);
            if (hook_result.block) {
                item.result = {.ok = false, .content = hook_result.reason.empty()
                                                          ? "Tool execution was blocked"
                                                          : hook_result.reason};
            }
        }
    }

    // Execute tools in parallel (only those not blocked by before hooks).
    std::vector<std::thread> threads;
    threads.reserve(tool_calls.size());
    for (size_t i = 0; i < tool_calls.size(); ++i) {
        threads.emplace_back([this, &work_items, i]() {
            auto& item = work_items[i];
            if (item.result.ok) {
                item.result = execute_single_tool_raw(item.call, [](const std::string&) {});
            }
        });
    }

    for (auto& t : threads) {
        if (t.joinable()) {
            t.join();
        }
    }

    // Run afterToolCall hooks sequentially for all tools.
    for (size_t i = 0; i < tool_calls.size(); ++i) {
        auto& item = work_items[i];
        if (config_.after_tool_call && item.result.ok) {
            AfterToolCallContext ctx{
                .tool_name = item.call.name,
                .tool_call_id = item.call.id,
                .args = item.call.arguments_json,
                .result = item.result.content,
                .isError = !item.result.ok,
            };
            AfterToolCallResult hook_result = config_.after_tool_call(ctx);
            if (!hook_result.content.empty()) {
                item.result.content = hook_result.content;
            }
        }
    }

    for (const auto& item : work_items) {
        on_chunk("[tool: " + item.call.name + "] " +
                 (item.result.ok ? "done" : "failed: " + item.result.content) + "\n");
        emit_tool_result(item.call, item.result);
    }

    return true;
}

// ============================================================================
// Internal: active_tool_definitions
// ============================================================================

std::vector<ToolDefinition> AgentSession::active_tool_definitions() const {
    if (config_.no_tools) return {};
    const auto all = tools_.build_tool_definitions();
    if (active_tool_names_.empty()) return all;

    std::vector<ToolDefinition> active;
    for (const auto& def : all) {
        if (std::find(active_tool_names_.begin(), active_tool_names_.end(), def.name)
                != active_tool_names_.end()) {
            active.push_back(def);
        }
    }
    return active;
}

// ============================================================================
// Internal: check_and_compact
// ============================================================================

bool AgentSession::check_and_compact(const ChunkCallback& on_chunk) {
    if (!config_.auto_compaction) return true;
    if (!should_compact(::coding_agent::total_context_tokens(messages_),
                        config_.context_size,
                        config_.compaction_reserve_tokens)) {
        return true;
    }

    on_chunk("\n[COMPACT] summarizing history...\n");

    AgentEvent start_ev{};
    start_ev.type = AgentEvent::Type::CompactionStart;
    emit_event(start_ev.type, start_ev);

    std::string error;
    CompactionStats stats;
    const bool ok = compact_history(
        messages_,
        provider_,
        current_model_,
        config_.compaction_keep_recent_tokens,
        config_.compaction_reserve_tokens,
        &last_compaction_file_ops_,
        &stats,
        error
    );

    AgentEvent end_ev{};
    end_ev.type              = AgentEvent::Type::CompactionEnd;
    end_ev.compaction_success = ok && stats.did_compact;

    if (!ok) {
        end_ev.compaction_error = error;
        emit_event(end_ev.type, end_ev);
        if (config_.compaction_fail_fast) return false;
        on_chunk("[COMPACT] skipped: " + error + "\n");
        try {
            const auto path = session_->getSessionFile();
            if (!path.has_value()) {
                return true;
            }
            std::ofstream out(path.value(), std::ios::app);
            out << nlohmann::json{
                    {"type", "compaction_skipped"},
                    {"reason", error},
                    {"tokens_before", ::coding_agent::total_context_tokens(messages_)}
                }.dump()
                << "\n";
        } catch (...) {}
        return true;
    }

    if (stats.did_compact) {
        on_chunk("[COMPACT] " + std::to_string(stats.tokens_before) + " -> " +
                 std::to_string(stats.tokens_after) + " tokens\n");
        last_compaction_stats_ = stats;
        compaction_count_++;

        end_ev.tokens_before      = stats.tokens_before;
        end_ev.tokens_after       = stats.tokens_after;
        end_ev.compaction_summary = stats.summary;

        nlohmann::json details{
            {"read_files", sorted_file_list(stats.file_ops.read_files)},
            {"modified_files", sorted_file_list(stats.file_ops.modified_files)},
            {"tokens_after", stats.tokens_after},
        };
        session_->appendCompaction(stats.summary, stats.first_kept_entry_id, stats.tokens_before,
                                  std::make_optional(details));
        last_compaction_file_ops_.read_files.clear();
        last_compaction_file_ops_.modified_files.clear();
        for (const auto& p : stats.file_ops.read_files) {
            last_compaction_file_ops_.read_files.insert(p);
        }
        for (const auto& p : stats.file_ops.modified_files) {
            last_compaction_file_ops_.modified_files.insert(p);
        }
    }

    emit_event(end_ev.type, end_ev);
    return true;
}

void AgentSession::resolve_retry() {
    {
        std::lock_guard<std::mutex> lock(retry_mutex_);
        retry_in_progress_ = false;
    }
    retry_cv_.notify_all();
}

// ============================================================================
// Queue API
// ============================================================================

void AgentSession::steer(const std::string& text,
                         const std::vector<std::string>& images) {
    steering_queue_.enqueue(text, images);
    emit_queue_update();
}

void AgentSession::followUp(const std::string& text,
                            const std::vector<std::string>& images) {
    follow_up_queue_.enqueue(text, images);
    emit_queue_update();
}

void AgentSession::clear_steering_queue() {
    steering_queue_.clear();
    emit_queue_update();
}

void AgentSession::clear_follow_up_queue() {
    follow_up_queue_.clear();
    emit_queue_update();
}

void AgentSession::clear_all_queues() {
    steering_queue_.clear();
    follow_up_queue_.clear();
    pending_next_turn_messages_.clear();
    emit_queue_update();
}

bool AgentSession::has_queued_messages() const {
    return steering_queue_.has_items() || follow_up_queue_.has_items();
}

QueueMode AgentSession::steering_mode() const {
    return steering_queue_.mode();
}

void AgentSession::set_steering_mode(QueueMode mode) {
    steering_queue_.set_mode(mode);
}

QueueMode AgentSession::follow_up_mode() const {
    return follow_up_queue_.mode();
}

void AgentSession::set_follow_up_mode(QueueMode mode) {
    follow_up_queue_.set_mode(mode);
}

void AgentSession::waitForRetry() {
    if (!retry_in_progress_) return;
    std::unique_lock<std::mutex> lock(retry_mutex_);
    retry_cv_.wait(lock, [this] { return !retry_in_progress_; });
}

void AgentSession::sendCustomMessage(const std::string& custom_type,
                                     const std::string& content,
                                     const std::string& /*display*/,
                                     const std::vector<std::string>& details,
                                     CustomMessageDelivery delivery) {
    std::string message;
    if (!details.empty()) {
        message = content + " [" + custom_type + "]";
    } else {
        message = content;
    }

    switch (delivery) {
        case CustomMessageDelivery::Steer:
            steer(message);
            break;
        case CustomMessageDelivery::FollowUp:
            followUp(message);
            break;
        case CustomMessageDelivery::NextTurn:
            pending_next_turn_messages_.push_back(message);
            break;
    }
    emit_queue_update();
}

void AgentSession::emit_queue_update() {
    // Only emit if either queue actually has items.
    if (!steering_queue_.has_items() && !follow_up_queue_.has_items()) {
        return;
    }

    AgentEvent ev{};
    ev.type = AgentEvent::Type::QueueUpdate;

    // Build display from queue state without draining
    steering_messages_.clear();
    for (const auto& [text, _] : steering_queue_.items()) {
        steering_messages_.push_back(text);
    }
    follow_up_messages_.clear();
    for (const auto& [text, _] : follow_up_queue_.items()) {
        follow_up_messages_.push_back(text);
    }

    ev.steering_messages = steering_messages_;
    ev.follow_up_messages = follow_up_messages_;
    emit_event(ev.type, ev);
}

// ============================================================================
// Retry Helpers
// ============================================================================

bool AgentSession::is_retryable_error(const std::string& error_text) const {
    if (error_text.empty()) return false;

    std::string lower(error_text);
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return std::tolower(c); });

    // Rate limit indicators
    if (lower.find("rate limit") != std::string::npos ||
        lower.find("ratelimit") != std::string::npos ||
        lower.find("429") != std::string::npos) {
        return true;
    }

    // Overloaded indicators
    if (lower.find("overloaded") != std::string::npos ||
        lower.find("503") != std::string::npos ||
        lower.find("service unavailable") != std::string::npos) {
        return true;
    }

    // Server error indicators
    if (lower.find("500") != std::string::npos ||
        lower.find("internal error") != std::string::npos) {
        return true;
    }

    // Timeout
    if (lower.find("timeout") != std::string::npos) {
        return true;
    }

    // Connection errors
    if (lower.find("connection reset") != std::string::npos ||
        lower.find("connection refused") != std::string::npos) {
        return true;
    }

    return false;
}

bool AgentSession::handle_retryable_error(const ChunkCallback& on_chunk,
                                          std::atomic<bool>* cancel_flag) {
    if (!config_.retry_enabled) return false;
    if (retry_in_progress_) return false;

    const std::string& error_text = last_turn_debug_.provider_error;
    if (!is_retryable_error(error_text)) return false;

    while (true) {
        retry_in_progress_ = true;
        ++retry_attempt_;

        if (retry_attempt_ > config_.retry_max_retries) {
            retry_in_progress_ = false;
            retry_attempt_     = 0;
            retry_deadline_.reset();
            retry_cv_.notify_all();
            on_chunk("[retry] Failed after " + std::to_string(config_.retry_max_retries) +
                     " attempts: " + error_text + "\n");
            return false;
        }

        if (retry_attempt_ == 1 && config_.retry_timeout_ms > 0) {
            retry_deadline_ = std::chrono::steady_clock::now() +
                              std::chrono::milliseconds(config_.retry_timeout_ms);
        }

        int delay_ms = config_.retry_base_delay_ms;
        for (int i = 1; i < retry_attempt_; ++i) {
            delay_ms *= 2;
        }
        delay_ms = std::min(delay_ms, config_.retry_max_retry_delay_ms);

        if (retry_deadline_.has_value()) {
            const auto now = std::chrono::steady_clock::now();
            if (now >= retry_deadline_.value()) {
                retry_in_progress_ = false;
                retry_attempt_     = 0;
                retry_deadline_.reset();
                retry_cv_.notify_all();
                on_chunk("[retry] Aborted: exceeded retry_timeout_ms\n");
                return false;
            }
            const auto remaining_ms =
                std::chrono::duration_cast<std::chrono::milliseconds>(retry_deadline_.value() - now)
                    .count();
            delay_ms = std::min(delay_ms, static_cast<int>(std::max<std::int64_t>(0, remaining_ms)));
        }

        AgentEvent start_ev{};
        start_ev.type                = AgentEvent::Type::AutoRetryStart;
        start_ev.retry_attempt       = retry_attempt_;
        start_ev.retry_max_attempts  = config_.retry_max_retries;
        start_ev.retry_delay_ms      = delay_ms;
        start_ev.retry_error_message = error_text;
        emit_event(start_ev.type, start_ev);

        on_chunk("[retry #" + std::to_string(retry_attempt_) + "/" +
                 std::to_string(config_.retry_max_retries) + "] Waiting " +
                 std::to_string(delay_ms) + "ms before retry...\n");

        {
            const auto sleep_until =
                std::chrono::steady_clock::now() + std::chrono::milliseconds(delay_ms);
            while (std::chrono::steady_clock::now() < sleep_until) {
                if (cancel_flag && cancel_flag->load()) {
                    retry_in_progress_ = false;
                    retry_attempt_     = 0;
                    retry_deadline_.reset();
                    retry_cv_.notify_all();
                    on_chunk("[retry] Cancelled\n");
                    return false;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
        }

        ChatResponse response;
        std::string error;
        const bool success = call_provider(
            messages_,
            active_tool_definitions(),
            response,
            error,
            on_chunk,
            cancel_flag
        );

        if (success) {
            ChatMessage assistant{
                .role         = "assistant",
                .content      = response.content,
                .tool_calls   = response.tool_calls,
                .usage_tokens = response.completion_tokens,
            };
            assistant.entry_id = session_->appendMessage(assistant);
            messages_.push_back(assistant);

            AgentEvent end_ev{};
            end_ev.type           = AgentEvent::Type::AutoRetryEnd;
            end_ev.retry_success  = true;
            end_ev.retry_attempt  = retry_attempt_;
            emit_event(end_ev.type, end_ev);

            on_chunk("[retry] Success after " + std::to_string(retry_attempt_) + " attempts\n");

            retry_attempt_ = 0;
            retry_deadline_.reset();
            resolve_retry();
            return true;
        }

        AgentEvent fail_ev{};
        fail_ev.type             = AgentEvent::Type::AutoRetryEnd;
        fail_ev.retry_success    = false;
        fail_ev.retry_attempt    = retry_attempt_;
        fail_ev.retry_final_error = error;
        emit_event(fail_ev.type, fail_ev);

        if (is_retryable_error(error) && retry_attempt_ < config_.retry_max_retries) {
            if (retry_deadline_.has_value() &&
                std::chrono::steady_clock::now() >= retry_deadline_.value()) {
                on_chunk("[retry] Aborted: exceeded retry_timeout_ms\n");
                retry_attempt_ = 0;
                retry_deadline_.reset();
                resolve_retry();
                return false;
            }
            resolve_retry();
            continue;
        }

        on_chunk("[retry] Failed after " + std::to_string(retry_attempt_) + " attempts: " + error +
                 "\n");

        retry_attempt_ = 0;
        retry_deadline_.reset();
        resolve_retry();
        return false;
    }
}

// ============================================================================
// Internal: emit_event
// ============================================================================

void AgentSession::emit_event(AgentEvent::Type /*type*/, const AgentEvent& event) {
    if (event_handler_) event_handler_(event);
}

void AgentSession::emit_abort_event() {
    abort_requested_.store(false, std::memory_order_release);
    AgentEvent ev{};
    ev.type = AgentEvent::Type::Abort;
    emit_event(ev.type, ev);
}

// ============================================================================
// Sub-Agent Task Delegation (Phase 11)
// ============================================================================

bool AgentSession::looks_like_multi_task(const std::string& user_input) const {
    // Simple heuristic: count distinct action verbs or imperative clauses
    // Look for patterns like "implement X, add tests, update docs"
    int clause_count = 0;
    bool in_clause = false;

    for (size_t i = 0; i < user_input.size(); ++i) {
        char c = user_input[i];
        if (c == ',' || c == ';' || c == '\n') {
            if (in_clause) {
                clause_count++;
                in_clause = false;
            }
        } else if (!std::isspace(static_cast<unsigned char>(c))) {
            in_clause = true;
        }
    }
    if (in_clause) clause_count++;

    // Also count commas as separators
    int comma_count = 0;
    for (char c : user_input) {
        if (c == ',') comma_count++;
    }

    // Multi-task if we have multiple distinct clauses or several comma-separated items
    return clause_count >= 3 || comma_count >= 2;
}

bool AgentSession::decomposeIntoSubtasks(const std::string& user_input,
                                          std::vector<SubTask>& subtasks,
                                          std::string& error) {
    // Build a decomposition request message
    ChatMessage decomp_request{
        .role = "user",
        .content = "Decompose the following user request into subtasks. Respond with ONLY a JSON object matching this schema:\n\n"
                   "{\n"
                   "  \"description\": \"Brief summary\",\n"
                   "  \"subtasks\": [\n"
                   "    {\n"
                   "      \"id\": \"1\",\n"
                   "      \"description\": \"What to do\",\n"
                   "      \"context_files\": [\"file1\"],\n"
                   "      \"expected_artifacts\": [\"output1\"],\n"
                   "      \"dependencies\": [],\n"
                   "      \"priority\": 1\n"
                   "    }\n"
                   "  ]\n"
                   "}\n\n"
                   "Rules:\n"
                   "- Each subtask should be self-contained and executable by a single coding-agent process\n"
                   "- Dependencies must form a DAG (no cycles)\n"
                   "- Priority is used for execution order (lower number = execute first)\n"
                   "- If the request is a single task, respond with a single subtask\n"
                   "- Context files are files the sub-agent should read before starting\n"
                   "- Expected artifacts are files the sub-agent is expected to create or modify\n\n"
                   "User request: " + user_input,
    };

    ChatResponse response;
    std::string call_error;

    // Call provider without tools for decomposition
    if (!call_provider({decomp_request}, {}, response, call_error, [](const std::string&) {}, nullptr)) {
        error = "Decomposition LLM call failed: " + call_error;
        return false;
    }

    // Parse JSON response
    try {
        nlohmann::json j = nlohmann::json::parse(response.content, nullptr, false);
        if (j.is_discarded()) {
            error = "Decomposition response is not valid JSON";
            return false;
        }

        if (!j.contains("subtasks") || !j["subtasks"].is_array()) {
            error = "Decomposition response missing 'subtasks' array";
            return false;
        }

        // Store decomposition entry in session
        nlohmann::json subtasks_json = j["subtasks"];
        std::string description = j.value("description", "Task decomposition");
        session_->appendSubTaskDecompositionEntry(description, subtasks_json);

        // Extract subtasks with all fields
        for (const auto& st : j["subtasks"]) {
            SubTask task;
            task.id = st.value("id", std::to_string(subtasks.size() + 1));
            task.description = st.value("description", "");

            if (st.contains("context_files") && st["context_files"].is_array()) {
                for (const auto& cf : st["context_files"]) {
                    task.contextFiles.push_back(cf.get<std::string>());
                }
            }

            if (st.contains("expected_artifacts") && st["expected_artifacts"].is_array()) {
                for (const auto& ea : st["expected_artifacts"]) {
                    task.expectedArtifacts.push_back(ea.get<std::string>());
                }
            }

            if (st.contains("dependencies") && st["dependencies"].is_array()) {
                for (const auto& dep : st["dependencies"]) {
                    task.dependencies.push_back(dep.get<std::string>());
                }
            }

            task.priority = st.value("priority", static_cast<int>(subtasks.size() + 1));

            if (!task.description.empty()) {
                subtasks.push_back(std::move(task));
            }
        }

        return !subtasks.empty();

    } catch (const std::exception& ex) {
        error = std::string("Failed to parse decomposition JSON: ") + ex.what();
        return false;
    }
}

bool AgentSession::executeSubtasks(const std::vector<SubTask>& subtasks,
                                    const ChunkCallback& on_chunk,
                                    std::atomic<bool>* cancel_flag) {
    // Validate the dependency graph
    auto validationError = validateSubtaskDag(subtasks);
    if (validationError.has_value()) {
        on_chunk("[DECOMPOSE] DAG validation failed: " + *validationError + "\n");
        on_chunk("[DECOMPOSE] Cannot execute subtasks with invalid dependencies\n");
        return false;
    }

    // Compute topological execution order
    auto order = topologicalSortSubtasks(subtasks);
    if (!order.has_value()) {
        on_chunk("[DECOMPOSE] Topological sort failed: dependency graph is invalid\n");
        return false;
    }

    // Build a map from task ID to task data for quick lookup
    std::unordered_map<std::string, const SubTask*> taskMap;
    for (const auto& task : subtasks) {
        taskMap[task.id] = &task;
    }

    // Build server config from parent's connection
    ServerConfig serverConfig;
    serverConfig.baseUrl = config_.base_url;
    serverConfig.modelId = current_model_;
    serverConfig.apiKey = config_.api_key;
    serverConfig.contextFiles = {};

    // Determine binary path: resolve from argv[0] if available, otherwise fallback to PATH lookup
    std::string binaryPath = "coding-agent";  // Fallback: relies on PATH
    // In production, this should be resolved from the parent binary path

    // Determine GPU lock path: use config if set, otherwise derive from server URL
    // to ensure different servers get different lock files (critical for multi-GPU setups).
    std::string gpuLockPath;
    if (!config_.gpu_lock_path.empty()) {
        gpuLockPath = config_.gpu_lock_path;
    } else {
        // Derive a unique lock path per server by hashing the base URL.
        // This prevents lock conflicts when running subagents on different machines
        // connecting to different GPUs.
        const std::string serverKey = config_.base_url.empty() ? "default" : config_.base_url;
        std::hash<std::string> hasher;
        const size_t hash = hasher(serverKey);
        const std::string defaultDir = config_.cwd + "/.pi";
        gpuLockPath = defaultDir + "/gpu-" + std::to_string(hash) + ".lock";
    }
    namespace fs = std::filesystem;
    fs::create_directories(fs::path(gpuLockPath).parent_path());

    const std::string parentSessionDir = session_->getSessionDir();
    const std::string parentSessionId = session_->getSessionId();

    // Create subtask entries in parent session (all start as pending)
    for (const auto& task : subtasks) {
        session_->appendSubTaskEntry(
            task.id,
            task.description,
            "pending",
            "",
            task.dependencies
        );
    }

    // Execute subtasks in topological order
    int completedCount = 0;
    int totalCount = static_cast<int>(order->size());

    for (const auto& taskId : *order) {
        // Check for abort
        if (cancel_flag && cancel_flag->load()) {
            return false;
        }

        const SubTask* task = taskMap[taskId];
        if (!task) {
            on_chunk("[DECOMPOSE] Internal error: task not found: " + taskId + "\n");
            continue;
        }

        // Update subtask state to running
        auto entries = session_->getEntries();
        for (auto it = entries.rbegin(); it != entries.rend(); ++it) {
            if (auto* ste = std::get_if<SubTaskEntry>(&*it)) {
                if (ste->subtaskId == taskId && ste->state == "pending") {
                    ste->state = "running";
                    break;
                }
            }
        }

        on_chunk("\n[SUBTASK " + std::to_string(completedCount + 1) + "/" + std::to_string(totalCount) + "] " + task->description + "\n");

        // Generate child session filename with parent session ID, subtask ID, and timestamp
        const auto now = std::chrono::system_clock::now();
        const auto ms = std::chrono::time_point_cast<std::chrono::milliseconds>(now);
        const std::string timestampPrefix = std::to_string(ms.time_since_epoch().count());

        // Simple ID generation for the child session
        std::string childSessionId = timestampPrefix + "_";
        for (int i = 0; i < 8; ++i) {
            childSessionId += "0123456789abcdef"[rand() % 16];
        }

        // Filename format: subtask-<parent-session-id>-<subtask-id>_<timestamp>_<child-session-id>.jsonl
        const std::string childSessionFilename = "subtask-" + parentSessionId + "-" +
            taskId + "_" + timestampPrefix + "_" + childSessionId + ".jsonl";
        const fs::path childSessionPath = fs::path(parentSessionDir) / childSessionFilename;

        // Spawn child process (default 30 min timeout)
        SubAgentResult result = SubAgent::spawn(
            binaryPath,
            task->description,
            task->contextFiles,
            serverConfig,
            parentSessionDir,
            gpuLockPath,
            config_.max_tokens,
            config_.temperature,
            1800000  // 30 minutes
        );

        // Update subtask state
        entries = session_->getEntries();
        for (auto it = entries.rbegin(); it != entries.rend(); ++it) {
            if (auto* ste = std::get_if<SubTaskEntry>(&*it)) {
                if (ste->subtaskId == taskId && ste->state == "running") {
                    ste->state = result.success ? "completed" : "failed";
                    ste->sessionId = result.sessionId;
                    ste->resultSummary = result.lastAssistantMessage;
                    if (!result.success && result.error.has_value()) {
                        ste->errorMessage = *result.error;
                    }
                    break;
                }
            }
        }

        // Inject result into parent session
        if (result.success) {
            std::string summary = result.lastAssistantMessage;
            if (summary.length() > 500) {
                summary = summary.substr(0, 500) + "...";
            }
            on_chunk("[tool: subtask_" + taskId + "] completed\n"
                     "→ Result: " + summary + "\n");

            session_->appendCustomMessageEntry(
                "subtask_result",
                "[tool: subtask_" + taskId + "] completed\n"
                "→ Result: " + result.lastAssistantMessage,
                false
            );
        } else {
            on_chunk("[tool: subtask_" + taskId + "] failed: "
                     + (result.error.value_or("unknown error")) + "\n");

            session_->appendCustomMessageEntry(
                "subtask_result",
                "[tool: subtask_" + taskId + "] failed: "
                + (result.error.value_or("unknown error")),
                false
            );
        }

        completedCount++;
    }

    return true;
}

bool AgentSession::decomposeAndExecute(const std::string& user_input,
                                        const ChunkCallback& on_chunk,
                                        std::atomic<bool>* cancel_flag) {
    // Step 1: Check if this looks like a multi-task request
    if (!looks_like_multi_task(user_input)) {
        return false;  // Not a multi-task request, handle normally
    }

    on_chunk("\n[DECOMPOSE] Detecting subtasks...\n");

    // Step 2: Decompose into subtasks
    std::vector<SubTask> subtasks;
    std::string error;

    if (!decomposeIntoSubtasks(user_input, subtasks, error)) {
        on_chunk("[DECOMPOSE] Failed: " + error + "\n");
        on_chunk("[DECOMPOSE] Falling back to single-task mode\n");
        return false;
    }

    on_chunk("[DECOMPOSE] Found " + std::to_string(subtasks.size()) + " subtask(s)\n");

    // Step 3: Execute subtasks
    on_chunk("\n[EXECUTE] Starting subtask execution...\n");
    bool success = executeSubtasks(subtasks, on_chunk, cancel_flag);
    on_chunk("\n[EXECUTE] Subtask execution " + std::string(success ? "completed" : "interrupted") + "\n");

    return success;
}

}  // namespace coding_agent


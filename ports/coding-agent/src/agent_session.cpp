#include "agent_session.hpp"

#include <algorithm>
#include <atomic>
#include <fstream>
#include <nlohmann/json.hpp>
#include <sstream>

#include "compaction.hpp"
#include "context_loader.hpp"
#include "file_ops.hpp"
#include "session.hpp"
#include "session_entry.hpp"
#include "system_prompt.hpp"

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
                           SessionManager& session)
    : config_(config),
      provider_(provider),
      tools_(tools),
      session_(session),
      current_model_(config.model),
      current_thinking_level_(ThinkingLevel::Off) {

    active_tool_names_ = parse_tool_name_list(config_.initial_active_tools);

    SessionContext ctx = session_.buildSessionContext();
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
        session_.appendMessage(sys);
    } else {
        current_system_prompt_ = messages_.front().content;
    }
}

AgentSession::~AgentSession() = default;

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
    session_.appendModelChange(config_.provider, model_id);
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
    session_.appendThinkingLevelChange(thinking_level_to_string(level));
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

        CompactionEvent event{
            .tokens_before         = stats.tokens_before,
            .tokens_after          = stats.tokens_after,
            .first_kept_index      = -1,
            .first_kept_entry_id   = stats.first_kept_entry_id,
            .summary               = stats.summary,
            .read_files            = sorted_file_list(stats.file_ops.read_files),
            .modified_files        = sorted_file_list(stats.file_ops.modified_files),
        };
        nlohmann::json details{
            {"read_files", event.read_files},
            {"modified_files", event.modified_files},
            {"tokens_after", event.tokens_after},
        };
        session_.appendCompaction(event.summary, event.first_kept_entry_id, event.tokens_before,
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

// ============================================================================
// Session Info
// ============================================================================

std::string AgentSession::session_id()   const { return session_.getSessionId(); }
std::string AgentSession::session_path() const {
    const auto p = session_.getSessionFile();
    return p.has_value() ? *p : "";
}
int         AgentSession::compaction_count()      const { return compaction_count_; }
int         AgentSession::total_context_tokens()  const {
    return ::coding_agent::total_context_tokens(messages_);
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
    user.entry_id = session_.appendMessage(user);
    messages_.push_back(user);

    AgentEvent ts_ev{};
    ts_ev.type        = AgentEvent::Type::TurnStart;
    ts_ev.turn_index  = turn_index_;
    emit_event(ts_ev.type, ts_ev);

    for (int iter = 0; iter < config_.max_tool_iterations; ++iter) {
        // Signal before each model call (drives between-tool animation resume).
        AgentEvent mc_ev{};
        mc_ev.type = AgentEvent::Type::ModelCallStart;
        emit_event(mc_ev.type, mc_ev);

        ChatResponse response;
        std::string error;
        if (!call_provider(messages_, active_tool_definitions(), response, error, on_chunk, cancel_flag)) {
            if (error == "interrupted") {
                if (!messages_.empty() && messages_.back().role == "user") {
                    messages_.pop_back();
                }
                return false;
            }
            AgentEvent err_ev{};
            err_ev.type          = AgentEvent::Type::Error;
            err_ev.error_message = error;
            emit_event(err_ev.type, err_ev);
            return false;
        }

        ChatMessage assistant{
            .role         = "assistant",
            .content      = response.content,
            .tool_calls   = response.tool_calls,
            .usage_tokens = response.completion_tokens,
        };
        assistant.entry_id = session_.appendMessage(assistant);
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

        if (response.tool_calls.empty()) break;

        execute_tools(response.tool_calls, on_chunk);
    }

    ++turn_index_;
    AgentEvent te_ev{};
    te_ev.type       = AgentEvent::Type::TurnEnd;
    te_ev.turn_index = turn_index_ - 1;
    emit_event(te_ev.type, te_ev);

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

bool AgentSession::execute_tools(const std::vector<ToolCall>& tool_calls,
                                 const ChunkCallback& on_chunk) {
    for (const auto& call : tool_calls) {
        AgentEvent call_ev{};
        call_ev.type         = AgentEvent::Type::ToolCall;
        call_ev.tool_name    = call.name;
        call_ev.tool_call_id = call.id;
        call_ev.tool_args    = call.arguments_json;
        emit_event(call_ev.type, call_ev);

        on_chunk("[tool: " + call.name + "] " + describe_tool_call(call.name, call.arguments_json) + "\n");

        const ToolResult result = tools_.dispatch(call.name, call.arguments_json, config_.cwd);

        on_chunk("[tool: " + call.name + "] " +
                 (result.ok ? "done" : "failed: " + result.content) + "\n");

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
        tool_msg.entry_id = session_.appendMessage(tool_msg);
        messages_.push_back(tool_msg);
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
            const auto path = session_.getSessionFile();
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

        CompactionEvent event{
            .tokens_before       = stats.tokens_before,
            .tokens_after        = stats.tokens_after,
            .first_kept_index    = -1,
            .first_kept_entry_id = stats.first_kept_entry_id,
            .summary             = stats.summary,
            .read_files          = sorted_file_list(stats.file_ops.read_files),
            .modified_files      = sorted_file_list(stats.file_ops.modified_files),
        };
        nlohmann::json details{
            {"read_files", event.read_files},
            {"modified_files", event.modified_files},
            {"tokens_after", event.tokens_after},
        };
        session_.appendCompaction(event.summary, event.first_kept_entry_id, event.tokens_before,
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

// ============================================================================
// Internal: emit_event
// ============================================================================

void AgentSession::emit_event(AgentEvent::Type /*type*/, const AgentEvent& event) {
    if (event_handler_) event_handler_(event);
}

}  // namespace coding_agent

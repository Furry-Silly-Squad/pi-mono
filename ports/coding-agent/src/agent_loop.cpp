#include "agent_loop.hpp"

#include <atomic>
#include <algorithm>
#include <functional>
#include <sstream>

namespace coding_agent {

RunResult run_agent_loop(
    const Config& config,
    Provider& provider,
    ToolRegistry& tools,
    std::vector<ChatMessage>& history,
    SessionStore& session,
    const std::string& user_input,
    const ChunkCallback& on_chunk,
    std::atomic<bool>* cancel_flag,
    const std::function<void()>& on_before_model_turn
) {
  ChatMessage user{
      .role = "user",
      .content = user_input,
      .tool_call_id = std::nullopt,
      .tool_calls = {},
  };
  user.entry_id = session.assign_entry_id();
  history.push_back(user);
  std::string persist_error;
  session.append(user, persist_error);

  for (int iteration = 0; iteration < config.max_tool_iterations; ++iteration) {
    if (on_before_model_turn) {
      on_before_model_turn();
    }

    // Save history state before this turn for potential rollback on interrupt
    const size_t history_size_before = history.size();

    const std::vector<ToolDefinition> request_tools =
        config.no_tools ? std::vector<ToolDefinition>{} : tools.build_tool_definitions();
    // Tool rounds (especially `edit`) emit large JSON in assistant.tool_calls.arguments.
    // Default max_tokens (~512–1024) truncates mid-JSON and breaks both stream and non-stream paths.
    constexpr int k_tool_round_min_tokens = 8192;
    int request_max_tokens = config.max_tokens;
    if (!request_tools.empty()) {
      request_max_tokens = std::max(request_max_tokens, k_tool_round_min_tokens);
    }

    ChatRequest request{
        .messages = history,
        .tools = request_tools,
        .model = config.model,
        .max_tokens = request_max_tokens,
        .temperature = config.temperature,
        .stream = config.stream,
    };

    ChatResponse response;
    std::string error;
    if (!provider.chat(request, response, on_chunk, error, cancel_flag)) {
      if (error == "interrupted") {
        // Rollback history: remove the user message we just added
        if (history.size() > history_size_before) {
          history.pop_back();
        }
        return {.ok = false, .output = "", .error = "interrupted"};
      }
      return {.ok = false, .output = "", .error = error};
    }

    ChatMessage assistant{
        .role = "assistant",
        .content = response.content,
        .tool_call_id = std::nullopt,
        .tool_calls = response.tool_calls,
        .usage_tokens = response.completion_tokens,
    };
    assistant.entry_id = session.assign_entry_id();
    history.push_back(assistant);
    session.append(assistant, persist_error);

    // Print per-turn token breakdown
    const int content_tok = response_content_tokens(response);
    const int tool_tok = response_tool_calls_tokens(response);
    const int turn_total = content_tok + tool_tok;
    std::ostringstream token_breakdown;
    token_breakdown << "→ " << turn_total << " tokens (content: " << content_tok << ", tool_calls: " << tool_tok << ")";
    if (!response.tool_calls.empty()) {
      token_breakdown << "\n  tools: ";
      for (size_t i = 0; i < response.tool_calls.size(); ++i) {
        if (i > 0) token_breakdown << ", ";
        token_breakdown << response.tool_calls[i].name;
      }
      token_breakdown << "\n";
    }
    on_chunk(token_breakdown.str());

    CompactionStats compaction_stats;
    if (should_compact(
            total_context_tokens(history),
            config.context_size,
            config.compaction_reserve_tokens
        )) {
      std::ostringstream compact_start;
      compact_start << "\n[COMPACT] summarizing history...\n";
      on_chunk(compact_start.str());
      if (!compact_history(
              history,
              provider,
              config.model,
              config.compaction_keep_recent_tokens,
              config.compaction_reserve_tokens,
              &compaction_stats,
              error
          )) {
        return {.ok = false, .output = "", .error = "Compaction failed: " + error};
      }

      if (compaction_stats.did_compact) {
        std::ostringstream compact_status;
        compact_status << "[COMPACT] " << compaction_stats.tokens_before << " -> " << compaction_stats.tokens_after
                       << " tokens\n";
        on_chunk(compact_status.str());
        std::string persist_error_ignored;
        CompactionEvent event{
            .tokens_before = compaction_stats.tokens_before,
            .tokens_after = compaction_stats.tokens_after,
            .first_kept_index = -1,
            .first_kept_entry_id = compaction_stats.first_kept_entry_id.empty() ? std::nullopt : std::optional<std::string>(compaction_stats.first_kept_entry_id),
            .summary = compaction_stats.summary,
        };
        session.append_compaction(event, persist_error_ignored);
        session.record_compaction(event);
      }
    }

    if (response.tool_calls.empty()) {
      return {.ok = true, .output = response.content, .error = ""};
    }

    for (const auto& call : response.tool_calls) {
      // Print tool execution status
      std::ostringstream tool_status;
      tool_status << "[tool: " << call.name << "] running...\n";
      on_chunk(tool_status.str());

      const ToolResult result = tools.dispatch(call.name, call.arguments_json, config.cwd);
      std::ostringstream payload;
      payload << (result.ok ? "ok" : "error") << ": " << result.content;

      // Print tool completion status
      std::ostringstream tool_done;
      tool_done << "[tool: " << call.name << "] " << (result.ok ? "done" : "failed: " + result.content) << "\n";
      on_chunk(tool_done.str());

      ChatMessage tool_message{
          .role = "tool",
          .content = payload.str(),
          .tool_call_id = call.id,
          .tool_calls = {},
      };
      tool_message.entry_id = session.assign_entry_id();
      history.push_back(tool_message);
      session.append(tool_message, persist_error);
    }
  }

  return {.ok = false, .output = "", .error = "tool loop iteration limit exceeded"};
}

}  // namespace coding_agent

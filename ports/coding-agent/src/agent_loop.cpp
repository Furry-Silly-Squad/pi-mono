#include "agent_loop.hpp"

#include <algorithm>
#include <sstream>

namespace coding_agent {

RunResult run_agent_loop(
    const Config& config,
    Provider& provider,
    ToolRegistry& tools,
    std::vector<ChatMessage>& history,
    SessionStore& session,
    const std::string& user_input,
    const ChunkCallback& on_chunk
) {
  ChatMessage user{
      .role = "user",
      .content = user_input,
      .tool_call_id = std::nullopt,
      .tool_calls = {},
  };
  history.push_back(user);
  std::string persist_error;
  session.append(user, persist_error);

  for (int iteration = 0; iteration < 20; ++iteration) {
    const std::vector<ToolDefinition> request_tools =
        config.no_tools ? std::vector<ToolDefinition>{} : tools.build_tool_definitions();
    int request_max_tokens = config.max_tokens;
    if (!request_tools.empty()) {
      request_max_tokens = std::max(request_max_tokens, 1024);
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
    if (!provider.chat(request, response, on_chunk, error)) {
      return {.ok = false, .output = "", .error = error};
    }

    ChatMessage assistant{
        .role = "assistant",
        .content = response.content,
        .tool_call_id = std::nullopt,
        .tool_calls = response.tool_calls,
    };
    history.push_back(assistant);
    session.append(assistant, persist_error);

    CompactionStats compaction_stats;
    if (should_compact(
            total_context_tokens(history),
            config.context_size,
            config.compaction_reserve_tokens
        )) {
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
        std::ostringstream status;
        status << "\n[compaction] " << compaction_stats.tokens_before << " -> " << compaction_stats.tokens_after
               << " tokens\n";
        on_chunk(status.str());
        std::string persist_error_ignored;
        session.append_compaction(
            CompactionEvent{
                .tokens_before = compaction_stats.tokens_before,
                .tokens_after = compaction_stats.tokens_after,
                .first_kept_index = compaction_stats.first_kept_index,
                .summary = compaction_stats.summary,
            },
            persist_error_ignored
        );
      }
    }

    if (response.tool_calls.empty()) {
      return {.ok = true, .output = response.content, .error = ""};
    }

    for (const auto& call : response.tool_calls) {
      const ToolResult result = tools.dispatch(call.name, call.arguments_json, config.cwd);
      std::ostringstream payload;
      payload << (result.ok ? "ok" : "error") << ": " << result.content;

      ChatMessage tool_message{
          .role = "tool",
          .content = payload.str(),
          .tool_call_id = call.id,
          .tool_calls = {},
      };
      history.push_back(tool_message);
      session.append(tool_message, persist_error);
    }
  }

  return {.ok = false, .output = "", .error = "tool loop iteration limit exceeded"};
}

}  // namespace coding_agent

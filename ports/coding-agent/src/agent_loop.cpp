#include "agent_loop.hpp"

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
    ChatRequest request{
        .messages = history,
        .tools = config.no_tools ? std::vector<ToolDefinition>{} : tools.build_tool_definitions(),
        .model = config.model,
        .max_tokens = config.max_tokens,
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

    if (should_compact(total_context_tokens(history), config.context_size)) {
      compact_history(history, provider, config.model, error);
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

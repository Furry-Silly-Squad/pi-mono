#include "compaction.hpp"

namespace coding_agent {

int approx_tokens(const std::string& text) {
  return static_cast<int>(text.size() / 4);
}

int total_context_tokens(const std::vector<ChatMessage>& messages) {
  int total = 0;
  for (const auto& message : messages) {
    total += approx_tokens(message.content);
    for (const auto& call : message.tool_calls) {
      total += approx_tokens(call.arguments_json);
    }
  }
  return total;
}

bool should_compact(int total_tokens, int context_size) {
  return total_tokens >= (context_size * 8 / 10);
}

bool compact_history(
    std::vector<ChatMessage>& messages,
    Provider& provider,
    const std::string& model,
    std::string& error
) {
  if (messages.size() < 4) {
    return true;
  }

  ChatRequest request{
      .messages = messages,
      .tools = {},
      .model = model,
      .max_tokens = 512,
      .temperature = 0.1f,
      .stream = false,
  };
  request.messages.push_back(
      ChatMessage{
          .role = "user",
          .content = "Summarize the conversation with key decisions, files changed, and pending work.",
          .tool_call_id = std::nullopt,
          .tool_calls = {},
      }
  );

  ChatResponse response;
  if (!provider.chat(request, response, [](const std::string&) {}, error)) {
    return false;
  }

  ChatMessage system_message = messages.front();
  messages.clear();
  messages.push_back(system_message);
  messages.push_back(
      ChatMessage{
          .role = "assistant",
          .content = "Conversation summary:\n" + response.content,
          .tool_call_id = std::nullopt,
          .tool_calls = {},
      }
  );
  return true;
}

}  // namespace coding_agent

#include "compaction.hpp"

#include <algorithm>

namespace coding_agent {
namespace {

int message_tokens(const ChatMessage& message) {
  int total = approx_tokens(message.content);
  for (const auto& call : message.tool_calls) {
    total += approx_tokens(call.arguments_json);
  }
  return total;
}

bool is_tool_result_message(const ChatMessage& message) {
  return message.role == "tool";
}

int find_first_kept_index(const std::vector<ChatMessage>& messages, int keep_recent_tokens) {
  if (messages.size() <= 2) {
    return static_cast<int>(messages.size());
  }
  int accumulated = 0;
  int cut = static_cast<int>(messages.size());
  for (int i = static_cast<int>(messages.size()) - 1; i >= 1; --i) {
    accumulated += message_tokens(messages[static_cast<size_t>(i)]);
    cut = i;
    if (accumulated >= keep_recent_tokens) {
      break;
    }
  }

  // Never split tool call/result sequence: if cut lands in tool results,
  // rewind to the corresponding assistant (or user) message.
  while (cut > 1 && is_tool_result_message(messages[static_cast<size_t>(cut)])) {
    --cut;
  }
  return std::max(1, cut);
}

}  // namespace

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

bool should_compact(int total_tokens, int context_size, int reserve_tokens) {
  return total_tokens >= (context_size - reserve_tokens);
}

bool compact_history(
    std::vector<ChatMessage>& messages,
    Provider& provider,
    const std::string& model,
    int keep_recent_tokens,
    int reserve_tokens,
    CompactionStats* stats,
    std::string& error
) {
  if (messages.size() < 4) {
    if (stats != nullptr) {
      stats->tokens_before = total_context_tokens(messages);
      stats->tokens_after = stats->tokens_before;
    }
    return true;
  }

  const int tokens_before = total_context_tokens(messages);

  const int first_kept_index = find_first_kept_index(messages, keep_recent_tokens);
  if (first_kept_index <= 1 || first_kept_index >= static_cast<int>(messages.size())) {
    if (stats != nullptr) {
      stats->tokens_before = tokens_before;
      stats->tokens_after = tokens_before;
    }
    return true;
  }

  std::vector<ChatMessage> to_summarize;
  to_summarize.reserve(static_cast<size_t>(first_kept_index - 1));
  for (int i = 1; i < first_kept_index; ++i) {
    to_summarize.push_back(messages[static_cast<size_t>(i)]);
  }

  ChatRequest request{
      .messages = to_summarize,
      .tools = {},
      .model = model,
      .max_tokens = std::max(256, reserve_tokens / 8),
      .temperature = 0.1f,
      .stream = false,
  };
  const std::string summary_user_prompt = R"(## Goal
## Constraints & Preferences
## Progress
### Done
### In Progress
### Blocked
## Key Decisions
## Next Steps
## Critical Context)";

  request.messages.push_back(
      ChatMessage{
          .role = "user",
          .content = summary_user_prompt,
          .tool_call_id = std::nullopt,
          .tool_calls = {},
      }
  );

  ChatResponse response;
  if (!provider.chat(request, response, [](const std::string&) {}, error)) {
    return false;
  }

  ChatMessage system_message = messages.front();
  std::vector<ChatMessage> compacted;
  compacted.reserve(messages.size() - static_cast<size_t>(first_kept_index) + 2);
  compacted.push_back(system_message);
  compacted.push_back(
      ChatMessage{
          .role = "assistant",
          .content = "Conversation summary:\n" + response.content,
          .tool_call_id = std::nullopt,
          .tool_calls = {},
      }
  );
  for (size_t i = static_cast<size_t>(first_kept_index); i < messages.size(); ++i) {
    compacted.push_back(messages[i]);
  }
  messages = std::move(compacted);

  if (stats != nullptr) {
    stats->tokens_before = tokens_before;
    stats->tokens_after = total_context_tokens(messages);
    stats->tokens_summarized = std::max(0, tokens_before - stats->tokens_after);
    stats->first_kept_index = first_kept_index;
    stats->did_compact = true;
    stats->summary = response.content;
  }
  return true;
}

}  // namespace coding_agent

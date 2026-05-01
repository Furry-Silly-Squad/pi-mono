#include "compaction.hpp"

#include <algorithm>

namespace coding_agent {
namespace {

int message_tokens(const ChatMessage& message) {
  // If the message carries usage data, use it for assistant messages.
  // Tool messages and user messages still use the char/4 heuristic.
  if (message.role == "assistant" && message.usage_tokens > 0) {
    return message.usage_tokens;
  }
  int total = approx_tokens(message.content);
  for (const auto& call : message.tool_calls) {
    total += approx_tokens(call.arguments_json);
  }
  return total;
}

bool is_tool_result_message(const ChatMessage& message) {
  return message.role == "tool";
}

// Find the index of the first message to keep, starting from the end of the
// history and accumulating tokens until keep_recent_tokens is reached.
//
// If boundary_start_entry_id is provided, the scan starts from the first
// message whose entry_id matches boundary_start_entry_id (i.e., the first
// message kept by the previous compaction). This ensures each compaction
// only summarizes the delta since the last compaction, not the full history.
int find_first_kept_index(
    const std::vector<ChatMessage>& messages,
    int keep_recent_tokens,
    const std::optional<std::string>& boundary_start_entry_id
) {
  if (messages.size() <= 2) {
    return static_cast<int>(messages.size());
  }

  // Determine the starting index for token accumulation.
  int start_idx = 1; // default: start after system prompt
  if (boundary_start_entry_id.has_value()) {
    for (int i = 1; i < static_cast<int>(messages.size()); ++i) {
      if (messages[static_cast<size_t>(i)].entry_id.has_value() &&
          messages[static_cast<size_t>(i)].entry_id.value() == boundary_start_entry_id.value()) {
        start_idx = i;
        break;
      }
    }
  }

  int accumulated = 0;
  int cut = static_cast<int>(messages.size());
  for (int i = static_cast<int>(messages.size()) - 1; i >= start_idx; --i) {
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
  return std::max(start_idx, cut);
}

// Find the entry_id of the first message kept by the most recent prior
// compaction. Returns std::nullopt if no prior compaction is found.
std::optional<std::string> find_last_compaction_boundary(
    const std::vector<ChatMessage>& messages
) {
  // Scan from the end to find the most recent compaction summary message.
  for (int i = static_cast<int>(messages.size()) - 1; i >= 0; --i) {
    const auto& msg = messages[static_cast<size_t>(i)];
    if (msg.role == "assistant" &&
        msg.entry_id.has_value() &&
        msg.content.find("Compaction summary (tokens before:") == 0) {
      return msg.entry_id.value();
    }
  }
  return std::nullopt;
}

// Find the previous compaction summary text in the message history.
std::string find_previous_summary(const std::vector<ChatMessage>& messages) {
  for (int i = static_cast<int>(messages.size()) - 1; i >= 0; --i) {
    const auto& msg = messages[static_cast<size_t>(i)];
    if (msg.role == "assistant" &&
        msg.content.find("Compaction summary (tokens before:") == 0) {
      // Extract the summary text after the "Compaction summary (tokens before: N):\n" prefix.
      const std::string prefix = "Compaction summary (tokens before: ";
      const size_t prefix_end = msg.content.find("):\n", prefix.size());
      if (prefix_end != std::string::npos) {
        return msg.content.substr(prefix_end + 3);
      }
    }
  }
  return "";
}

// The initial structured summary prompt.
const std::string SUMMARY_USER_PROMPT = R"(## Goal
## Constraints & Preferences
## Progress
### Done
### In Progress
### Blocked
## Key Decisions
## Next Steps
## Critical Context)";

// The update prompt variant that incorporates a previous summary.
const std::string SUMMARY_UPDATE_USER_PROMPT = R"(## Previous Summary
<previous-summary>
{PREVIOUS_SUMMARY}
</previous-summary>

## New Context
Summarize only the new developments since the previous summary. Merge
new information into the existing structure. Do not repeat content that
is already captured in the previous summary.

## Goal
## Constraints & Preferences
## Progress
### Done
### In Progress
### Blocked
## Key Decisions
## Next Steps
## Critical Context)";

}  // namespace

int approx_tokens(const std::string& text) {
  return static_cast<int>(text.size() / 4);
}

int response_content_tokens(const ChatResponse& response) {
  return approx_tokens(response.content);
}

int response_tool_calls_tokens(const ChatResponse& response) {
  int total = 0;
  for (const auto& call : response.tool_calls) {
    total += approx_tokens(call.arguments_json);
  }
  return total;
}

int total_context_tokens(const std::vector<ChatMessage>& messages) {
  int total = 0;
  for (const auto& message : messages) {
    total += message_tokens(message);
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

  // Task 3: Iterative boundary detection — find the prior compaction's
  // first_kept_entry_id so we only summarize the delta.
  const auto boundary_start = find_last_compaction_boundary(messages);

  const int first_kept_index = find_first_kept_index(messages, keep_recent_tokens, boundary_start);
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

  // Task 5: Iterative summary update — check for a prior compaction summary
  // in the history and use the update prompt if found.
  const std::string previous_summary = find_previous_summary(messages);
  std::string user_prompt_content;

  if (!previous_summary.empty()) {
    // Replace the placeholder with the actual previous summary.
    user_prompt_content = SUMMARY_UPDATE_USER_PROMPT;
    const std::string placeholder = "{PREVIOUS_SUMMARY}";
    const size_t pos = user_prompt_content.find(placeholder);
    if (pos != std::string::npos) {
      user_prompt_content.replace(pos, placeholder.size(), previous_summary);
    }
    stats->previous_summary = previous_summary;
  } else {
    user_prompt_content = SUMMARY_USER_PROMPT;
  }

  ChatRequest request{
      .messages = to_summarize,
      .tools = {},
      .model = model,
      .max_tokens = std::max(256, reserve_tokens / 8),
      .temperature = 0.1f,
      .stream = false,
  };
  request.messages.push_back(
      ChatMessage{
          .role = "user",
          .content = user_prompt_content,
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
    // Store the entry_id of the first kept message (at position first_kept_index
    // in the original messages, which is now at position 2 in the compacted vector).
    if (first_kept_index < static_cast<int>(messages.size())) {
      stats->first_kept_entry_id = messages[static_cast<size_t>(first_kept_index)].entry_id.value_or("");
    }
    stats->did_compact = true;
    stats->summary = response.content;
  }
  return true;
}

}  // namespace coding_agent

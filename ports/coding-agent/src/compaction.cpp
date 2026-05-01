#include "compaction.hpp"

#include <algorithm>

namespace coding_agent {
namespace {

/// Internal result of cut point analysis.
struct CutPointResult {
  int first_kept_index = -1;
  int turn_start_index = -1;   // -1 if not a split turn
  bool is_split_turn = false;
  std::string first_kept_entry_id;
};

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

bool is_valid_cut_point(const ChatMessage& message) {
  // Valid cut points: user messages, assistant messages (including compaction summaries).
  // Never cut at a tool result message — they must remain attached to their
  // preceding assistant call.
  return message.role == "user" || message.role == "assistant";
}

/// Task 1: Find all valid cut-point indices, sorted ascending.
std::vector<int> find_valid_cut_points(const std::vector<ChatMessage>& messages) {
  std::vector<int> result;
  for (int i = 0; i < static_cast<int>(messages.size()); ++i) {
    if (is_valid_cut_point(messages[static_cast<size_t>(i)])) {
      result.push_back(i);
    }
  }
  return result;
}

/// Task 2: Walk backwards from cut_index to find the closest preceding user message.
int find_turn_start_index(const std::vector<ChatMessage>& messages, int cut_index, int boundary_start) {
  for (int i = cut_index - 1; i >= boundary_start; --i) {
    if (messages[static_cast<size_t>(i)].role == "user") {
      return i;
    }
  }
  return -1;
}

/// Task 1: Two-pass cut point selection.
/// 1. Collect valid cut-point indices.
/// 2. Walk backwards accumulating tokens from the end, stop when budget reached.
/// 3. Pick the nearest valid cut point at or after the stopping index.
CutPointResult find_cut_point(
    const std::vector<ChatMessage>& messages,
    int keep_recent_tokens,
    const std::optional<std::string>& boundary_start_entry_id
) {
  CutPointResult result;

  if (messages.size() <= 2) {
    result.first_kept_index = static_cast<int>(messages.size());
    return result;
  }

  // Collect valid cut points.
  const auto valid_cuts = find_valid_cut_points(messages);

  // Determine starting index for token accumulation.
  int start_idx = 1;
  if (boundary_start_entry_id.has_value()) {
    for (int i = 1; i < static_cast<int>(messages.size()); ++i) {
      if (messages[static_cast<size_t>(i)].entry_id.has_value() &&
          messages[static_cast<size_t>(i)].entry_id.value() == boundary_start_entry_id.value()) {
        start_idx = i;
        break;
      }
    }
  }

  // Walk backwards accumulating tokens.
  int accumulated = 0;
  int stop_idx = static_cast<int>(messages.size());
  for (int i = static_cast<int>(messages.size()) - 1; i >= start_idx; --i) {
    accumulated += message_tokens(messages[static_cast<size_t>(i)]);
    stop_idx = i;
    if (accumulated >= keep_recent_tokens) {
      break;
    }
  }

  // Find the nearest valid cut point at or after stop_idx.
  int best_cut = static_cast<int>(messages.size());
  for (int vc : valid_cuts) {
    if (vc >= stop_idx && vc >= start_idx) {
      best_cut = vc;
      break;
    }
  }

  // If no valid cut point found at or after stop_idx, use the last valid one before stop_idx.
  if (best_cut >= static_cast<int>(messages.size())) {
    for (int i = static_cast<int>(valid_cuts.size()) - 1; i >= 0; --i) {
      if (valid_cuts[static_cast<size_t>(i)] < stop_idx && valid_cuts[static_cast<size_t>(i)] >= start_idx) {
        best_cut = valid_cuts[static_cast<size_t>(i)];
        break;
      }
    }
  }

  result.first_kept_index = std::max(start_idx, best_cut);

  // Task 2: Check for split turn.
  if (result.first_kept_index > start_idx) {
    const int turn_start = find_turn_start_index(messages, result.first_kept_index, start_idx);
    if (turn_start > 0) {
      // Check if the cut point falls within a turn (between user message and its assistant response).
      bool has_intermediate = false;
      for (int i = turn_start + 1; i < result.first_kept_index; ++i) {
        if (is_valid_cut_point(messages[static_cast<size_t>(i)])) {
          has_intermediate = true;
          break;
        }
      }
      if (!is_valid_cut_point(messages[static_cast<size_t>(result.first_kept_index)]) || has_intermediate) {
        result.is_split_turn = true;
        result.turn_start_index = turn_start;
      }
    }
  }

  // Store entry_id of the first kept message.
  if (result.first_kept_index < static_cast<int>(messages.size())) {
    result.first_kept_entry_id = messages[static_cast<size_t>(result.first_kept_index)].entry_id.value_or("");
  }

  return result;
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

// The turn prefix summary prompt for split-turn cases.
const std::string SUMMARY_TURN_PREFIX_PROMPT = R"(## Original Request
## Early Progress
## Context for Suffix)";

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

/// Find the entry_id of the first message kept by the most recent prior
/// compaction. Returns std::nullopt if no prior compaction is found.
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

/// Find the previous compaction summary text in the message history.
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

  // Iterative boundary detection — find the prior compaction's first_kept_entry_id.
  const auto boundary_start = find_last_compaction_boundary(messages);

  // Task 1 & 2 & 4: Use find_cut_point() for proper cut point analysis.
  const CutPointResult cut = find_cut_point(messages, keep_recent_tokens, boundary_start);

  if (cut.first_kept_index <= 1 || cut.first_kept_index >= static_cast<int>(messages.size())) {
    if (stats != nullptr) {
      stats->tokens_before = tokens_before;
      stats->tokens_after = tokens_before;
    }
    return true;
  }

  // Task 3: Split-turn prefix summarization.
  // If this is a split turn, we produce two summaries:
  // 1. A "turn prefix" summary covering the split turn.
  // 2. A main summary covering the pre-turn history.

  std::string turn_prefix_summary;
  std::vector<ChatMessage> to_summarize;

  if (cut.is_split_turn && cut.turn_start_index >= 0) {
    // Collect turn prefix messages (from turn_start to first_kept).
    std::vector<ChatMessage> turn_prefix_messages;
    for (int i = cut.turn_start_index; i < cut.first_kept_index; ++i) {
      turn_prefix_messages.push_back(messages[static_cast<size_t>(i)]);
    }

    // Summarize the turn prefix.
    ChatRequest prefix_request{
        .messages = turn_prefix_messages,
        .tools = {},
        .model = model,
        .max_tokens = std::max(128, reserve_tokens / 16),
        .temperature = 0.1f,
        .stream = false,
    };
    prefix_request.messages.push_back(
        ChatMessage{
            .role = "user",
            .content = SUMMARY_TURN_PREFIX_PROMPT,
            .tool_call_id = std::nullopt,
            .tool_calls = {},
        }
    );

    ChatResponse prefix_response;
    if (!provider.chat(prefix_request, prefix_response, [](const std::string&) {}, error)) {
      return false;
    }
    turn_prefix_summary = prefix_response.content;

    // Main summarization covers history from after system prompt to turn_start.
    to_summarize.reserve(static_cast<size_t>(cut.turn_start_index - 1));
    for (int i = 1; i < cut.turn_start_index; ++i) {
      to_summarize.push_back(messages[static_cast<size_t>(i)]);
    }
  } else {
    // Normal case: summarize from after system prompt to first_kept.
    to_summarize.reserve(static_cast<size_t>(cut.first_kept_index - 1));
    for (int i = 1; i < cut.first_kept_index; ++i) {
      to_summarize.push_back(messages[static_cast<size_t>(i)]);
    }
  }

  // Iterative summary update — check for a prior compaction summary.
  const std::string previous_summary = find_previous_summary(messages);
  std::string user_prompt_content;

  if (!previous_summary.empty()) {
    user_prompt_content = SUMMARY_UPDATE_USER_PROMPT;
    const std::string placeholder = "{PREVIOUS_SUMMARY}";
    const size_t pos = user_prompt_content.find(placeholder);
    if (pos != std::string::npos) {
      user_prompt_content.replace(pos, placeholder.size(), previous_summary);
    }
    if (stats != nullptr) {
      stats->previous_summary = previous_summary;
    }
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

  // Build compacted message list.
  ChatMessage system_message = messages.front();
  std::vector<ChatMessage> compacted;
  compacted.reserve(messages.size() - static_cast<size_t>(cut.first_kept_index) + 3);
  compacted.push_back(system_message);

  // Add turn prefix summary if split turn.
  if (!turn_prefix_summary.empty()) {
    compacted.push_back(
        ChatMessage{
            .role = "assistant",
            .content = "Turn context (split turn):\n" + turn_prefix_summary,
            .tool_call_id = std::nullopt,
            .tool_calls = {},
        }
    );
  }

  // Add main summary.
  compacted.push_back(
      ChatMessage{
          .role = "assistant",
          .content = "Conversation summary:\n" + response.content,
          .tool_call_id = std::nullopt,
          .tool_calls = {},
      }
  );

  // Keep messages from first_kept_index onward.
  for (size_t i = static_cast<size_t>(cut.first_kept_index); i < messages.size(); ++i) {
    compacted.push_back(messages[i]);
  }
  messages = std::move(compacted);

  if (stats != nullptr) {
    stats->tokens_before = tokens_before;
    stats->tokens_after = total_context_tokens(messages);
    stats->tokens_summarized = std::max(0, tokens_before - stats->tokens_after);
    stats->first_kept_entry_id = cut.first_kept_entry_id;
    stats->is_split_turn = cut.is_split_turn;
    if (cut.is_split_turn && cut.turn_start_index >= 0 &&
        cut.turn_start_index < static_cast<int>(messages.size())) {
      stats->turn_start_entry_id =
          messages[static_cast<size_t>(cut.turn_start_index)].entry_id.value_or("");
    }
    stats->did_compact = true;
    stats->summary = response.content;
  }
  return true;
}

}  // namespace coding_agent

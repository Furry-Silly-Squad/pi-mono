#pragma once

#include <optional>
#include <string>
#include <vector>

#include "providers/provider.hpp"

namespace coding_agent {

int approx_tokens(const std::string& text);
int total_context_tokens(const std::vector<ChatMessage>& messages);
int response_content_tokens(const ChatResponse& response);
int response_tool_calls_tokens(const ChatResponse& response);
bool should_compact(int total_tokens, int context_size, int reserve_tokens);

/// All statistics from a compaction run.
struct CompactionStats {
  int tokens_before = 0;
  int tokens_after = 0;
  int tokens_summarized = 0;
  std::string first_kept_entry_id;
  bool did_compact = false;
  std::string summary;
  std::string previous_summary;
  bool is_split_turn = false;
  std::string turn_start_entry_id;
};

/// Compact history: summarize older messages, keep recent ones verbatim.
bool compact_history(
    std::vector<ChatMessage>& messages,
    Provider& provider,
    const std::string& model,
    int keep_recent_tokens,
    int reserve_tokens,
    CompactionStats* stats,
    std::string& error
);

}  // namespace coding_agent

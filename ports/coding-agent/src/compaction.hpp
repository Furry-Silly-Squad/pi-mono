#pragma once

#include <string>
#include <vector>

#include "providers/provider.hpp"

namespace coding_agent {

int approx_tokens(const std::string& text);
int total_context_tokens(const std::vector<ChatMessage>& messages);
bool should_compact(int total_tokens, int context_size, int reserve_tokens);

struct CompactionStats {
  int tokens_before = 0;
  int tokens_after = 0;
  int tokens_summarized = 0;
  int first_kept_index = -1;
  bool did_compact = false;
  std::string summary;
};

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

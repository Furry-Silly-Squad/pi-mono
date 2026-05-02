#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "providers/provider.hpp"
#include "session_entry.hpp"

namespace coding_agent {

struct BranchSummaryData {
  std::string summary;
  std::vector<std::string> read_files;
  std::vector<std::string> modified_files;
};

/// Snippet + file-footer fallback (no LLM), used when reading another session file heuristically.
BranchSummaryData summarize_branch_session_file(const std::filesystem::path& session_path);

/// Entries from `old_leaf_id` back to `target_id` (exclusive of common ancestor); chronological order.
/// If `target_id` is empty, walks up to the session root (excluding the session header row).
std::vector<SessionEntry> collect_entries_for_branch_summary(
    const SessionManager& mgr,
    const std::string& old_leaf_id,
    const std::string& target_id
);

struct PreparedBranchEntries {
  std::vector<ChatMessage> messages;
  FileOps file_ops;
  int total_tokens = 0;
};

PreparedBranchEntries prepare_branch_entries(const std::vector<SessionEntry>& entries, int token_budget);

struct BranchSummaryResult {
  std::string summary;
  std::vector<std::string> read_files;
  std::vector<std::string> modified_files;
};

BranchSummaryResult generate_branch_summary(
    const std::vector<SessionEntry>& entries,
    Provider& provider,
    const std::string& model,
    int context_size,
    int reserve_tokens,
    std::string& error
);

}  // namespace coding_agent

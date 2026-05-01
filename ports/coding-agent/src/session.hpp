#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "file_ops.hpp"
#include "providers/provider.hpp"

namespace coding_agent {

struct CompactionEvent {
  int tokens_before = 0;
  int tokens_after = 0;
  int first_kept_index = -1;
  std::string first_kept_entry_id;
  std::string summary;
  std::vector<std::string> read_files;
  std::vector<std::string> modified_files;
};

struct BranchSummaryEvent {
  std::string summary;
  std::string source_session_id;
  std::vector<std::string> read_files;
  std::vector<std::string> modified_files;
  /// Leaf entry id on the source session when summarizing an abandoned branch (for injection logic).
  std::string handoff_source_leaf_id;
};

enum class SessionRowKind {
  SessionHeader,
  Message,
  Compaction,
  BranchSummary,
  CompactionSkipped,
};

/// One parsed row from a session JSONL file (tree node).
struct SessionNode {
  std::string id;
  std::string parent_id;
  SessionRowKind kind = SessionRowKind::Message;
  ChatMessage message{};
  std::optional<CompactionEvent> compaction;
  struct BranchRowData {
    std::string summary;
    std::string source_session_id;
    std::string handoff_source_leaf_id;
    std::vector<std::string> read_files;
    std::vector<std::string> modified_files;
  };
  std::optional<BranchRowData> branch;
};

/// Parsed session tree from a JSONL file (ids, parent links, leaf).
struct SessionGraph {
  std::unordered_map<std::string, SessionNode> nodes;
  /// Last row id in file order (any row type with an id).
  std::string leaf_id;

  [[nodiscard]] std::vector<std::string> get_branch(const std::string& entry_id) const;
  [[nodiscard]] std::string find_common_ancestor(const std::string& id_a, const std::string& id_b) const;
  [[nodiscard]] const SessionNode* get_node(const std::string& id) const;
};

/// Load session tree from JSONL (for branch summarization and traversal).
bool load_session_graph(const std::string& path, SessionGraph& out, std::string& error);

/// Most recently modified `.jsonl` session file in a directory, if any.
std::optional<std::filesystem::path> latest_session_path_in_dir(const std::string& session_dir);

class SessionStore {
 public:
  /// Normal constructor uses `~/.config/coding-agent/sessions` (or `$HOME`-relative fallback).
  /// If `session_dir_override` is non-empty, session files are stored there instead (used by tests).
  explicit SessionStore(std::string cwd, std::optional<std::string> session_dir_override = std::nullopt);

  std::string start_or_resume(const std::optional<std::string>& requested_id, bool force_new);
  bool append(const ChatMessage& message, std::string& error);
  bool append_compaction(const CompactionEvent& event, std::string& error);
  bool append_branch_summary(const BranchSummaryEvent& event, std::string& error);
  std::vector<ChatMessage> load_messages(std::string& error);

  // Assign entry_id to a message and return it
  std::string assign_entry_id();

  // Get the entry_id of the first kept message from the most recent compaction
  std::optional<std::string> get_last_compaction_first_kept_entry_id() const;
  const FileOps& get_last_compaction_file_ops() const;

  // Get compaction statistics
  int get_compaction_count() const;
  const CompactionEvent& get_last_compaction_event() const;
  void record_compaction(const CompactionEvent& event);

  // Get session ID
  std::string get_session_id() const;

  // Get session file path
  const std::string& get_session_path() const;

  [[nodiscard]] const std::string& session_dir() const;

  /// Populated after load_messages(); reflects persisted tree + rows appended this run (best-effort).
  [[nodiscard]] const SessionGraph& graph() const;

  [[nodiscard]] std::vector<std::string> get_branch(const std::string& entry_id) const;
  [[nodiscard]] std::string find_common_ancestor(const std::string& id_a, const std::string& id_b) const;

 private:
  std::string session_dir_;
  std::string session_id_;
  std::string session_path_;
  /// Last written row id in this session file (for parent linkage).
  std::string last_written_entry_id_;
  SessionGraph graph_;

  // Maps message index to entry_id for loaded messages
  std::vector<std::string> message_entry_ids_;
  // Stores first_kept_entry_id from each compaction row
  std::vector<std::string> compaction_first_kept_entry_ids_;
  // Stores compaction_skipped events for display
  struct CompactionSkippedEvent {
    std::string reason;
    int tokens_before;
  };
  std::vector<CompactionSkippedEvent> compaction_skipped_events_;
  // Tracks compaction events for /stats
  int compaction_count_ = 0;
  CompactionEvent last_compaction_event_;
  FileOps last_compaction_file_ops_;
};

}  // namespace coding_agent

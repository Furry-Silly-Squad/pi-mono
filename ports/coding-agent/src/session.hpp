#pragma once

#include <optional>
#include <string>
#include <vector>

#include "providers/provider.hpp"

namespace coding_agent {

struct CompactionEvent {
  int tokens_before = 0;
  int tokens_after = 0;
  int first_kept_index = -1;
  std::optional<std::string> first_kept_entry_id;
  std::string summary;
};

struct BranchSummaryEvent {
  std::string summary;
  std::string source_session_id;
};

class SessionStore {
 public:
  explicit SessionStore(std::string cwd);

  std::string start_or_resume(const std::optional<std::string>& requested_id, bool force_new);
  bool append(const ChatMessage& message, std::string& error);
  bool append_compaction(const CompactionEvent& event, std::string& error);
  bool append_branch_summary(const BranchSummaryEvent& event, std::string& error);
  std::vector<ChatMessage> load_messages(std::string& error);

  // Assign entry_id to a message and return it
  std::string assign_entry_id();

  // Get the entry_id of the first kept message from the most recent compaction
  std::optional<std::string> get_last_compaction_first_kept_entry_id() const;

  // Get compaction statistics
  int get_compaction_count() const;
  const CompactionEvent& get_last_compaction_event() const;
  void record_compaction(const CompactionEvent& event);

  // Get session ID
  std::string get_session_id() const;

 private:
  std::string session_dir_;
  std::string session_id_;
  std::string session_path_;
  // Maps message index to entry_id for loaded messages
  std::vector<std::string> message_entry_ids_;
  // Stores first_kept_entry_id from each compaction row
  std::vector<std::string> compaction_first_kept_entry_ids_;
  // Tracks compaction events for /stats
  int compaction_count_ = 0;
  CompactionEvent last_compaction_event_;
};

}  // namespace coding_agent

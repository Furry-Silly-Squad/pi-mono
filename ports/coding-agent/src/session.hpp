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
  std::vector<ChatMessage> load_messages(std::string& error) const;

 private:
  std::string session_dir_;
  std::string session_id_;
  std::string session_path_;
};

}  // namespace coding_agent

#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace coding_agent {

struct BranchSummaryData {
  std::string summary;
  std::vector<std::string> read_files;
  std::vector<std::string> modified_files;
};

BranchSummaryData summarize_branch_session_file(const std::filesystem::path& session_path);

}  // namespace coding_agent

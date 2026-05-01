#include "branch_summary.hpp"

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

#ifndef CODING_AGENT_TEST_FIXTURE_DIR
#error CODING_AGENT_TEST_FIXTURE_DIR must be defined by CMake
#endif

namespace {

bool fail(const char* msg) {
  std::cerr << "FAIL: " << msg << "\n";
  return false;
}

bool contains_all(const std::string& haystack, const std::vector<std::string>& needles) {
  for (const auto& n : needles) {
    if (haystack.find(n) == std::string::npos) {
      return false;
    }
  }
  return true;
}

}  // namespace

int main() {
  const std::filesystem::path fixture =
      std::filesystem::path(CODING_AGENT_TEST_FIXTURE_DIR) / "phase3_branch_session.jsonl";
  if (!std::filesystem::exists(fixture)) {
    std::cerr << "missing fixture: " << fixture.string() << "\n";
    return 2;
  }

  const coding_agent::BranchSummaryData data = coding_agent::summarize_branch_session_file(fixture);

  const std::vector<std::string> expect_read = {"/fixture/read.txt"};
  const std::vector<std::string> expect_modified = {"/fixture/edit.txt", "/fixture/write.txt"};
  if (data.read_files != expect_read) {
    return fail("read_files mismatch");
  }
  if (data.modified_files != expect_modified) {
    return fail("modified_files mismatch");
  }

  if (!contains_all(data.summary, {"## Files Read", "## Files Modified", "- /fixture/read.txt", "- /fixture/edit.txt",
                                   "- /fixture/write.txt"})) {
    return fail("summary should include file footer and paths");
  }

  std::cout << "coding-agent-branch-summary-test: ok\n";
  return 0;
}

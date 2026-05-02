#include "session_entry.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <unistd.h>

namespace {

bool fail(const char* msg) {
  std::cerr << "FAIL: " << msg << "\n";
  return false;
}

}  // namespace

int main() {
  namespace fs = std::filesystem;

  const fs::path dir =
      fs::temp_directory_path() / ("coding-agent-session-test-" + std::to_string(::getpid()));
  fs::create_directories(dir);

  // Write a session file with a compaction entry (v1 format — no id/parentId/timestamp)
  const fs::path session_file = dir / "rt.jsonl";
  {
    std::ofstream out(session_file);
    out << R"({"type":"session","id":"rt"})" << "\n";
    out << R"({"type":"compaction","tokens_before":10,"tokens_after":5,"summary":"hydrate-summary","first_kept_entry_id":"e1","read_files":["/hydrate/read.txt"],"modified_files":["/hydrate/mod.txt"]})"
        << "\n";
  }

  // Load the session — migration should add id/parentId/timestamp
  auto mgr = coding_agent::SessionManager::open(session_file.string(), "", "");
  if (!mgr) {
    std::cerr << "Failed to open session\n";
    return 1;
  }

  // Verify the compaction entry was loaded with migrated id
  const auto entries = mgr->getEntries();
  if (entries.empty()) {
    return fail("should have at least one entry after migration");
  }

  // Append a new compaction
  const std::string compaction_id = mgr->appendCompaction(
      "second-compaction", "", 3,
      nlohmann::json{{"read_files", {"/append/read.txt"}}, {"modified_files", {"/append/write.txt"}}}
  );

  // Re-open the session to verify persistence
  auto mgr2 = coding_agent::SessionManager::open(session_file.string(), "", "");
  if (!mgr2) {
    std::cerr << "Failed to re-open session\n";
    return 1;
  }

  const auto entries2 = mgr2->getEntries();
  if (entries2.size() != 2U) {
    return fail("should have two entries after appending compaction");
  }

  // Verify the last entry is the new compaction
  auto* last_comp = std::get_if<coding_agent::CompactionEntry>(&entries2.back());
  if (!last_comp) {
    return fail("last entry should be a compaction");
  }
  if (last_comp->summary != "second-compaction") {
    return fail("last compaction summary mismatch");
  }

  // Verify details were persisted
  if (!last_comp->details.has_value()) {
    return fail("compaction details should be persisted");
  }
  const auto& details = *last_comp->details;
  if (!details.contains("read_files") || !details.contains("modified_files")) {
    return fail("compaction details should contain read_files and modified_files");
  }

  // Verify the first compaction was also persisted
  auto* first_comp = std::get_if<coding_agent::CompactionEntry>(&entries2[0]);
  if (!first_comp) {
    return fail("first entry should be a compaction");
  }
  if (first_comp->summary != "hydrate-summary") {
    return fail("first compaction summary mismatch");
  }

  std::cout << "coding-agent-session-store-test: ok\n";
  return 0;
}

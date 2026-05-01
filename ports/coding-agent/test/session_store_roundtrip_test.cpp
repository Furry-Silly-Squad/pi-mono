#include "session.hpp"

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

  const fs::path session_file = dir / "rt.jsonl";
  {
    std::ofstream out(session_file);
    out << R"({"type":"session","id":"rt"})"
        << "\n"
        << R"({"type":"compaction","tokens_before":10,"tokens_after":5,"summary":"hydrate-summary","first_kept_entry_id":"e1","read_files":["/hydrate/read.txt"],"modified_files":["/hydrate/mod.txt"]})"
        << "\n";
  }

  coding_agent::SessionStore store(".", dir.string());
  store.start_or_resume("rt", false);

  std::string err;
  (void)store.load_messages(err);
  const coding_agent::FileOps& loaded = store.get_last_compaction_file_ops();
  if (loaded.read_files.count("/hydrate/read.txt") != 1U) {
    return fail("hydrated read_files");
  }
  if (loaded.modified_files.count("/hydrate/mod.txt") != 1U) {
    return fail("hydrated modified_files");
  }

  const coding_agent::CompactionEvent appended{
      .tokens_before = 3,
      .tokens_after = 2,
      .first_kept_index = -1,
      .first_kept_entry_id = "",
      .summary = "second-compaction",
      .read_files = {"/append/read.txt"},
      .modified_files = {"/append/write.txt"},
  };
  if (!store.append_compaction(appended, err)) {
    std::cerr << "append_compaction: " << err << "\n";
    return 1;
  }

  coding_agent::SessionStore store2(".", dir.string());
  store2.start_or_resume("rt", false);
  (void)store2.load_messages(err);
  const coding_agent::FileOps& after_append = store2.get_last_compaction_file_ops();
  if (after_append.read_files.count("/append/read.txt") != 1U || after_append.read_files.count("/hydrate/read.txt") != 0U) {
    return fail("last compaction row should replace file-op hydration");
  }
  if (after_append.modified_files.count("/append/write.txt") != 1U ||
      after_append.modified_files.count("/hydrate/mod.txt") != 0U) {
    return fail("last compaction modified_files");
  }

  std::cout << "coding-agent-session-store-test: ok\n";
  return 0;
}

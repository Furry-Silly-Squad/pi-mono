// Unit tests for EditTool: unique replacement and atomic temp-file + rename write path.

#include "tools/edit_tool.hpp"

#include <cstdlib>
#include <unistd.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <set>
#include <string>

namespace fs = std::filesystem;

namespace {

bool fail(const char* msg) {
  std::cerr << "FAIL: " << msg << "\n";
  return false;
}

#define EXPECT(cond, msg) \
  do {                    \
    if (!(cond)) return fail(msg); \
  } while (0)

fs::path make_temp_dir(const std::string& tag) {
  static int counter = 0;
  ++counter;
  fs::path dir =
      fs::temp_directory_path() /
      ("coding-agent-edit-tool-test-" + std::to_string(::getpid()) + "-" + tag + "-" + std::to_string(counter));
  fs::create_directories(dir);
  return dir;
}

std::set<std::string> tmp_marker_files_in_dir(const fs::path& dir) {
  std::set<std::string> names;
  if (!fs::exists(dir)) {
    return names;
  }
  for (const fs::directory_entry& ent : fs::directory_iterator(dir)) {
    if (!ent.is_regular_file()) continue;
    const std::string fn = ent.path().filename().string();
    if (fn.find(".tmp-") != std::string::npos) {
      names.insert(fn);
    }
  }
  return names;
}

bool test_edit_atomic_success_no_leftover_tmp() {
  const fs::path dir = make_temp_dir("atomic");
  const fs::path file = dir / "sample.txt";
  {
    std::ofstream out(file);
    EXPECT(out.is_open(), "create fixture file");
    out << "hello ALPHA world";
  }

  coding_agent::EditTool tool;
  const std::string args = R"({"path":"sample.txt","old_string":"ALPHA","new_string":"beta"})";
  const coding_agent::ToolResult r = tool.execute(args, dir.string());
  EXPECT(r.ok, "edit should succeed");
  EXPECT(r.content.find("Edited file:") != std::string::npos, "success message expected");

  std::string got;
  {
    std::ifstream in(file);
    std::stringstream ss;
    ss << in.rdbuf();
    got = ss.str();
  }
  EXPECT(got == "hello beta world", "file content should match after replace");

  const auto markers = tmp_marker_files_in_dir(dir);
  EXPECT(markers.empty(), "no .tmp- marker files should remain after successful rename");

  return true;
}

bool test_edit_missing_file() {
  const fs::path dir = make_temp_dir("missing");
  coding_agent::EditTool tool;
  const std::string args = R"({"path":"nope.txt","old_string":"a","new_string":"b"})";
  const coding_agent::ToolResult r = tool.execute(args, dir.string());
  EXPECT(!r.ok, "missing file should fail");
  EXPECT(r.content.find("Unable to open file") != std::string::npos, "open error expected");
  return true;
}

bool test_edit_old_string_not_found() {
  const fs::path dir = make_temp_dir("notfound");
  const fs::path file = dir / "a.txt";
  {
    std::ofstream out(file);
    out << "zzz";
  }
  coding_agent::EditTool tool;
  const std::string args = R"({"path":"a.txt","old_string":"nope","new_string":"x"})";
  const coding_agent::ToolResult r = tool.execute(args, dir.string());
  EXPECT(!r.ok, "should fail when old_string missing");
  EXPECT(r.content == "old_string not found", "specific error");
  return true;
}

bool test_edit_duplicate_old_string() {
  const fs::path dir = make_temp_dir("dup");
  const fs::path file = dir / "b.txt";
  {
    std::ofstream out(file);
    out << "xaxax";
  }
  coding_agent::EditTool tool;
  const std::string args = R"({"path":"b.txt","old_string":"x","new_string":"y"})";
  const coding_agent::ToolResult r = tool.execute(args, dir.string());
  EXPECT(!r.ok, "should fail when old_string not unique");
  EXPECT(r.content.find("multiple") != std::string::npos, "multiple occurrences error");
  return true;
}

using TestFn = bool (*)();

}  // namespace

int main() {
  const TestFn tests[] = {
      test_edit_atomic_success_no_leftover_tmp,
      test_edit_missing_file,
      test_edit_old_string_not_found,
      test_edit_duplicate_old_string,
  };
  for (TestFn t : tests) {
    if (!t()) {
      return 1;
    }
  }
  std::cerr << "edit_tool_test: all tests passed\n";
  return 0;
}

// Unit tests for bash_command_looks_destructive heuristics.

#include "tools/bash_destructive.hpp"

#include <iostream>
#include <string>

namespace {

bool fail(const char* msg) {
  std::cerr << "FAIL: " << msg << "\n";
  return false;
}

#define EXPECT(cond, msg) \
  do {                    \
    if (!(cond)) return fail(msg); \
  } while (0)

bool test_rm_detected() {
  EXPECT(coding_agent::bash_command_looks_destructive("rm -rf ./build"), "rm -rf");
  EXPECT(coding_agent::bash_command_looks_destructive("  rm foo"), "rm with leading space");
  return true;
}

bool test_safe_command_not_flagged() {
  EXPECT(!coding_agent::bash_command_looks_destructive("echo hello"), "echo");
  EXPECT(!coding_agent::bash_command_looks_destructive("ls -la"), "ls");
  return true;
}

bool test_git_reset_hard() {
  EXPECT(coding_agent::bash_command_looks_destructive("git reset --hard HEAD~1"), "git reset --hard");
  return true;
}

using TestFn = bool (*)();

}  // namespace

int main() {
  const TestFn tests[] = {
      test_rm_detected,
      test_safe_command_not_flagged,
      test_git_reset_hard,
  };
  for (TestFn t : tests) {
    if (!t()) {
      return 1;
    }
  }
  std::cerr << "bash_destructive_test: all tests passed\n";
  return 0;
}

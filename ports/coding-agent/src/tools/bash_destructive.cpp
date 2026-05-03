#include "tools/bash_destructive.hpp"

#include <array>
#include <cctype>
#include <string>

namespace coding_agent {
namespace {

std::string to_lower(std::string value) {
  for (char& ch : value) {
    ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
  }
  return value;
}

}  // namespace

bool bash_command_looks_destructive(const std::string& command) {
  const std::string lower = to_lower(command);
  const std::array<std::string, 14> patterns = {
      "rm ",
      "rm -",
      "rmdir ",
      "mv -f ",
      "dd if=",
      "mkfs",
      "git reset --hard",
      "git clean -fd",
      "git clean -xdf",
      "git checkout --",
      "chmod -r ",
      "chown -r ",
      "truncate -s 0",
      ": >",
  };
  for (const auto& pattern : patterns) {
    if (lower.find(pattern) != std::string::npos) {
      return true;
    }
  }
  return false;
}

}  // namespace coding_agent

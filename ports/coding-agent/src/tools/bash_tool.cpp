#include "tools/bash_tool.hpp"

#include <array>
#include <cctype>
#include <cstdio>
#include <iostream>
#include <memory>
#include <string>

#include <nlohmann/json.hpp>

namespace coding_agent {
namespace {

std::string to_lower(std::string value) {
  for (char& ch : value) {
    ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
  }
  return value;
}

bool contains_destructive_command(const std::string& command) {
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

bool ask_user_to_confirm(const std::string& command) {
  std::cout << "Destructive bash command requested:\n";
  std::cout << command << "\n";
  std::cout << "Allow execution? [y/N]: " << std::flush;
  std::string answer;
  if (!std::getline(std::cin, answer)) {
    return false;
  }
  const std::string normalized = to_lower(answer);
  return normalized == "y" || normalized == "yes";
}

}  // namespace

std::string BashTool::name() const {
  return "bash";
}

std::string BashTool::description() const {
  return "Execute a shell command in cwd";
}

std::string BashTool::parameters_schema() const {
  return R"({"type":"object","properties":{"command":{"type":"string"}},"required":["command"]})";
}

ToolResult BashTool::execute(const std::string& args_json, const std::string& cwd) {
  try {
    const auto args = nlohmann::json::parse(args_json);
    const std::string command = args.at("command").get<std::string>();
    if (contains_destructive_command(command) && !ask_user_to_confirm(command)) {
      return {.ok = false, .content = "Blocked destructive command: user confirmation not granted"};
    }
    const std::string wrapped = "cd \"" + cwd + "\" && " + command + " 2>&1";

    std::array<char, 4096> buffer{};
    std::string output;
    std::unique_ptr<FILE, decltype(&pclose)> pipe(popen(wrapped.c_str(), "r"), pclose);
    if (!pipe) {
      return {.ok = false, .content = "Failed to spawn shell process"};
    }
    while (fgets(buffer.data(), static_cast<int>(buffer.size()), pipe.get()) != nullptr) {
      output += buffer.data();
      if (output.size() > 100000) {
        output = output.substr(0, 100000) + "\n...[truncated]";
        break;
      }
    }
    return {.ok = true, .content = output};
  } catch (const std::exception& ex) {
    return {.ok = false, .content = ex.what()};
  }
}

}  // namespace coding_agent

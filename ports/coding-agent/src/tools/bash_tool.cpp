#include "tools/bash_tool.hpp"

#include <array>
#include <cstdio>
#include <memory>
#include <string>

#include <nlohmann/json.hpp>

namespace coding_agent {

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

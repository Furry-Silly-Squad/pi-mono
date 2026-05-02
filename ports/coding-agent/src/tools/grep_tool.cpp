#include "tools/grep_tool.hpp"

#include <array>
#include <cstdio>
#include <memory>

#include <nlohmann/json.hpp>

namespace coding_agent {

std::string GrepTool::name() const {
  return "grep";
}

std::string GrepTool::description() const {
  return "Search files with ripgrep";
}

std::string GrepTool::parameters_schema() const {
  return R"({"type":"object","properties":{"pattern":{"type":"string"},"path":{"type":"string"}},"required":["pattern"]})";
}

ToolResult GrepTool::execute(const std::string& args_json, const std::string& cwd) {
  try {
    const auto args = nlohmann::json::parse(args_json);
    const std::string pattern = args.at("pattern").get<std::string>();
    const std::string path = args.value("path", ".");
    const std::string command = "cd \"" + cwd + "\" && rg -n -- \"" + pattern + "\" \"" + path + "\" 2>&1";

    std::array<char, 4096> buffer{};
    std::string output;
    auto pclose_deleter = [](FILE* fp) { pclose(fp); };
    std::unique_ptr<FILE, decltype(pclose_deleter)> pipe(popen(command.c_str(), "r"), pclose_deleter);
    if (!pipe) {
      return {.ok = false, .content = "Failed to start rg"};
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

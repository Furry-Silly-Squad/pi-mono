#include "tools/find_tool.hpp"

#include <filesystem>
#include <sstream>

#include <nlohmann/json.hpp>

namespace coding_agent {

std::string FindTool::name() const {
  return "find";
}

std::string FindTool::description() const {
  return "Find files recursively by name substring";
}

std::string FindTool::parameters_schema() const {
  return R"({"type":"object","properties":{"pattern":{"type":"string"},"path":{"type":"string"}},"required":["pattern"]})";
}

ToolResult FindTool::execute(const std::string& args_json, const std::string& cwd) {
  try {
    const auto args = nlohmann::json::parse(args_json);
    const std::string pattern = args.at("pattern").get<std::string>();
    const std::filesystem::path root = std::filesystem::path(cwd) / args.value("path", ".");
    std::ostringstream out;

    size_t count = 0;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(root)) {
      if (!entry.is_regular_file() && !entry.is_directory()) {
        continue;
      }
      const std::string filename = entry.path().filename().string();
      if (filename.find(pattern) != std::string::npos) {
        out << entry.path().string() << "\n";
        ++count;
        if (count >= 1000) {
          out << "...[truncated]\n";
          break;
        }
      }
    }
    return {.ok = true, .content = out.str()};
  } catch (const std::exception& ex) {
    return {.ok = false, .content = ex.what()};
  }
}

}  // namespace coding_agent

#include "tools/ls_tool.hpp"

#include <filesystem>
#include <sstream>

#include <nlohmann/json.hpp>

namespace coding_agent {

std::string LsTool::name() const {
  return "ls";
}

std::string LsTool::description() const {
  return "List directory entries";
}

std::string LsTool::parameters_schema() const {
  return R"({"type":"object","properties":{"path":{"type":"string"}}})";
}

ToolResult LsTool::execute(const std::string& args_json, const std::string& cwd) {
  try {
    const auto args = nlohmann::json::parse(args_json.empty() ? "{}" : args_json);
    const std::filesystem::path path = std::filesystem::path(cwd) / args.value("path", ".");
    std::ostringstream out;
    size_t count = 0;
    for (const auto& entry : std::filesystem::directory_iterator(path)) {
      out << entry.path().filename().string();
      if (entry.is_directory()) {
        out << "/";
      }
      out << "\n";
      ++count;
      if (count >= 1000) {
        out << "...[truncated]\n";
        break;
      }
    }
    return {.ok = true, .content = out.str()};
  } catch (const std::exception& ex) {
    return {.ok = false, .content = ex.what()};
  }
}

}  // namespace coding_agent

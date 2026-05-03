#include "tools/write_tool.hpp"

#include <filesystem>
#include <fstream>

#include <nlohmann/json.hpp>

namespace coding_agent {

std::string WriteTool::name() const {
  return "write";
}

std::string WriteTool::description() const {
  return "Write text contents to a file";
}

std::string WriteTool::parameters_schema() const {
  return R"({"type":"object","properties":{"path":{"type":"string"},"content":{"type":"string"}},"required":["path","content"]})";
}

ToolExecutionMode WriteTool::execution_mode() const {
  return ToolExecutionMode::Sequential;
}

ToolResult WriteTool::execute(const std::string& args_json, const std::string& cwd) {
  try {
    const auto args = nlohmann::json::parse(args_json);
    const auto path = std::filesystem::path(cwd) / args.at("path").get<std::string>();
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path);
    output << args.at("content").get<std::string>();
    return {.ok = true, .content = "Wrote file: " + path.string()};
  } catch (const std::exception& ex) {
    return {.ok = false, .content = ex.what()};
  }
}

}  // namespace coding_agent

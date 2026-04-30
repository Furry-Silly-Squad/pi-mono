#include "tools/read_tool.hpp"

#include <filesystem>
#include <fstream>
#include <sstream>

#include <nlohmann/json.hpp>

namespace coding_agent {

std::string ReadTool::name() const {
  return "read";
}

std::string ReadTool::description() const {
  return "Read file contents with optional line range";
}

std::string ReadTool::parameters_schema() const {
  return R"({"type":"object","properties":{"path":{"type":"string"},"offset":{"type":"integer"},"limit":{"type":"integer"}},"required":["path"]})";
}

ToolResult ReadTool::execute(const std::string& args_json, const std::string& cwd) {
  try {
    const auto args = nlohmann::json::parse(args_json);
    const auto path = std::filesystem::path(cwd) / args.at("path").get<std::string>();
    std::ifstream input(path);
    if (!input.is_open()) {
      return {.ok = false, .content = "Unable to open file: " + path.string()};
    }
    std::stringstream buffer;
    buffer << input.rdbuf();
    return {.ok = true, .content = buffer.str()};
  } catch (const std::exception& ex) {
    return {.ok = false, .content = ex.what()};
  }
}

}  // namespace coding_agent

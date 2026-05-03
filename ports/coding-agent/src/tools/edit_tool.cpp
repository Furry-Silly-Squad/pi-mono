#include "tools/edit_tool.hpp"

#include <filesystem>
#include <fstream>
#include <sstream>

#include <nlohmann/json.hpp>

namespace coding_agent {

std::string EditTool::name() const {
  return "edit";
}

std::string EditTool::description() const {
  return "Replace unique text in a file";
}

std::string EditTool::parameters_schema() const {
  return R"({"type":"object","properties":{"path":{"type":"string"},"old_string":{"type":"string"},"new_string":{"type":"string"}},"required":["path","old_string","new_string"]})";
}

ToolResult EditTool::execute(const std::string& args_json, const std::string& cwd) {
  try {
    const auto args = nlohmann::json::parse(args_json);
    const auto path = std::filesystem::path(cwd) / args.at("path").get<std::string>();
    const std::string old_string = args.at("old_string").get<std::string>();
    const std::string new_string = args.at("new_string").get<std::string>();

    std::ifstream input(path);
    if (!input.is_open()) {
      return {.ok = false, .content = "Unable to open file: " + path.string()};
    }
    std::stringstream buffer;
    buffer << input.rdbuf();
    std::string content = buffer.str();

    const size_t first = content.find(old_string);
    if (first == std::string::npos) {
      return {.ok = false, .content = "old_string not found"};
    }
    const size_t second = content.find(old_string, first + old_string.size());
    if (second != std::string::npos) {
      return {.ok = false, .content = "old_string appears multiple times"};
    }
    content.replace(first, old_string.size(), new_string);

    std::ofstream output(path);
    if (!output.is_open()) {
      return {.ok = false, .content = "Unable to open file for writing: " + path.string()};
    }
    output << content;
    output.flush();
    if (!output.good()) {
      return {.ok = false, .content = "Failed to write file: " + path.string()};
    }
    return {.ok = true, .content = "Edited file: " + path.string()};
  } catch (const std::exception& ex) {
    return {.ok = false, .content = ex.what()};
  }
}

}  // namespace coding_agent

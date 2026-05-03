#include "tools/edit_tool.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>

#include <nlohmann/json.hpp>

namespace coding_agent {
namespace {

std::string make_temp_suffix() {
  static thread_local std::mt19937 rng(std::random_device{}());
  std::uniform_int_distribution<unsigned int> dist(0, 0xFFFFFFU);
  return ".tmp-" + std::to_string(
                      std::chrono::steady_clock::now().time_since_epoch().count()) +
         "-" + std::to_string(dist(rng));
}

ToolResult write_file_atomic(const std::filesystem::path& path, const std::string& content) {
  const std::filesystem::path temp_path = path.parent_path() / (path.filename().string() + make_temp_suffix());

  {
    std::ofstream output(temp_path, std::ios::binary | std::ios::trunc);
    if (!output.is_open()) {
      return {.ok = false, .content = "Unable to open temp file for writing: " + temp_path.string()};
    }
    output << content;
    output.flush();
    if (!output.good()) {
      std::error_code ignored;
      std::filesystem::remove(temp_path, ignored);
      return {.ok = false, .content = "Failed to write temp file: " + temp_path.string()};
    }
  }

  std::error_code rename_error;
  std::filesystem::rename(temp_path, path, rename_error);
  if (rename_error) {
    std::error_code ignored;
    std::filesystem::remove(temp_path, ignored);
    return {.ok = false, .content = "Failed to atomically replace file: " + rename_error.message()};
  }
  return {.ok = true, .content = "Edited file: " + path.string()};
}

}  // namespace

std::string EditTool::name() const {
  return "edit";
}

std::string EditTool::description() const {
  return "Replace unique text in a file";
}

std::string EditTool::parameters_schema() const {
  return R"({"type":"object","properties":{"path":{"type":"string"},"old_string":{"type":"string"},"new_string":{"type":"string"}},"required":["path","old_string","new_string"]})";
}

ToolExecutionMode EditTool::execution_mode() const {
  return ToolExecutionMode::Sequential;
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

    return write_file_atomic(path, content);
  } catch (const std::exception& ex) {
    return {.ok = false, .content = ex.what()};
  }
}

}  // namespace coding_agent

#include "file_ops.hpp"

#include <algorithm>
#include <nlohmann/json.hpp>
#include <sstream>

namespace coding_agent {
namespace {

void extract_from_tool_call(const ToolCall& call, FileOps& file_ops) {
  if (call.name != "read" && call.name != "write" && call.name != "edit") {
    return;
  }

  const auto args = nlohmann::json::parse(call.arguments_json, nullptr, false);
  if (args.is_discarded() || !args.contains("path") || !args.at("path").is_string()) {
    return;
  }
  const std::string path = args.at("path").get<std::string>();
  if (path.empty()) {
    return;
  }

  if (call.name == "read") {
    file_ops.read_files.insert(path);
    return;
  }

  file_ops.modified_files.insert(path);
}

}  // namespace

FileOps extract_file_ops_from_messages(const std::vector<ChatMessage>& messages) {
  FileOps file_ops;
  for (const auto& message : messages) {
    if (message.role != "assistant") {
      continue;
    }
    for (const auto& call : message.tool_calls) {
      extract_from_tool_call(call, file_ops);
    }
  }
  return file_ops;
}

void merge_file_ops(FileOps& dst, const FileOps& src) {
  dst.read_files.insert(src.read_files.begin(), src.read_files.end());
  dst.modified_files.insert(src.modified_files.begin(), src.modified_files.end());
}

std::vector<std::string> sorted_file_list(const std::unordered_set<std::string>& files) {
  std::vector<std::string> result(files.begin(), files.end());
  std::sort(result.begin(), result.end());
  return result;
}

std::string build_file_ops_footer(const FileOps& file_ops) {
  const auto read_files = sorted_file_list(file_ops.read_files);
  const auto modified_files = sorted_file_list(file_ops.modified_files);
  if (read_files.empty() && modified_files.empty()) {
    return "";
  }

  std::ostringstream out;
  if (!read_files.empty()) {
    out << "\n\n## Files Read\n";
    for (const auto& path : read_files) {
      out << "- " << path << "\n";
    }
  }
  if (!modified_files.empty()) {
    out << "\n## Files Modified\n";
    for (const auto& path : modified_files) {
      out << "- " << path << "\n";
    }
  }
  return out.str();
}

std::optional<std::filesystem::path> latest_session_path_in_dir(const std::string& session_dir) {
  namespace fs = std::filesystem;
  if (!fs::exists(session_dir) || !fs::is_directory(session_dir)) {
    return std::nullopt;
  }
  fs::file_time_type newest_time;
  std::optional<fs::path> latest;
  for (const auto& entry : fs::directory_iterator(session_dir)) {
    if (!entry.is_regular_file() || entry.path().extension() != ".jsonl") {
      continue;
    }
    if (!latest.has_value() || entry.last_write_time() > newest_time) {
      newest_time = entry.last_write_time();
      latest = entry.path();
    }
  }
  return latest;
}

}  // namespace coding_agent

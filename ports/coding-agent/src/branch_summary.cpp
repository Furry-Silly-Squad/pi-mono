#include "branch_summary.hpp"

#include <algorithm>
#include <fstream>
#include <string>
#include <vector>

#include "file_ops.hpp"
#include <nlohmann/json.hpp>

namespace coding_agent {

BranchSummaryData summarize_branch_session_file(const std::filesystem::path& session_path) {
  std::ifstream input(session_path);
  std::vector<std::string> lines;
  std::string line;
  while (std::getline(input, line)) {
    if (!line.empty()) {
      lines.push_back(line);
    }
  }

  std::vector<ChatMessage> messages;
  for (const auto& row_line : lines) {
    const auto row = nlohmann::json::parse(row_line, nullptr, false);
    if (row.is_discarded() || row.value("type", "") != "message") {
      continue;
    }
    ChatMessage message{
        .role = row.value("role", ""),
        .content = row.value("content", ""),
        .tool_call_id = std::nullopt,
        .tool_calls = {},
    };
    if (row.contains("tool_calls") && row.at("tool_calls").is_array()) {
      for (const auto& tc : row.at("tool_calls")) {
        message.tool_calls.push_back(
            ToolCall{
                .id = tc.value("id", ""),
                .name = tc.value("name", ""),
                .arguments_json = tc.value("arguments_json", "{}"),
            }
        );
      }
    }
    messages.push_back(std::move(message));
  }

  std::vector<std::string> snippets;
  for (int i = static_cast<int>(messages.size()) - 1; i >= 0 && snippets.size() < 6; --i) {
    const auto& message = messages[static_cast<size_t>(i)];
    const std::string role = message.role;
    const std::string raw_content = message.content;
    if (role != "user" && role != "assistant") {
      continue;
    }
    std::string content = raw_content;
    if (content.size() > 180) {
      content = content.substr(0, 180) + "...";
    }
    snippets.push_back("- " + role + ": " + content);
  }
  std::reverse(snippets.begin(), snippets.end());

  const FileOps file_ops = extract_file_ops_from_messages(messages);
  const auto read_files = sorted_file_list(file_ops.read_files);
  const auto modified_files = sorted_file_list(file_ops.modified_files);

  if (snippets.empty()) {
    return BranchSummaryData{
        .summary = "Previous session had no user/assistant messages to summarize.",
        .read_files = read_files,
        .modified_files = modified_files,
    };
  }

  std::string summary = "Recent context from previous session:\n";
  for (const auto& snippet : snippets) {
    summary += snippet + "\n";
  }
  summary += build_file_ops_footer(file_ops);
  return BranchSummaryData{
      .summary = summary,
      .read_files = read_files,
      .modified_files = modified_files,
  };
}

}  // namespace coding_agent

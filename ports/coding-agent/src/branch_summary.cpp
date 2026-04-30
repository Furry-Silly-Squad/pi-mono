#include "branch_summary.hpp"

#include <algorithm>
#include <fstream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace coding_agent {

std::string summarize_branch_session_file(const std::filesystem::path& session_path) {
  std::ifstream input(session_path);
  std::vector<std::string> lines;
  std::string line;
  while (std::getline(input, line)) {
    if (!line.empty()) {
      lines.push_back(line);
    }
  }

  std::vector<std::string> snippets;
  for (int i = static_cast<int>(lines.size()) - 1; i >= 0 && snippets.size() < 6; --i) {
    const auto row = nlohmann::json::parse(lines[static_cast<size_t>(i)], nullptr, false);
    if (row.is_discarded() || row.value("type", "") != "message") {
      continue;
    }
    const std::string role = row.value("role", "");
    if (role != "user" && role != "assistant") {
      continue;
    }
    std::string content = row.value("content", "");
    if (content.size() > 180) {
      content = content.substr(0, 180) + "...";
    }
    snippets.push_back("- " + role + ": " + content);
  }
  std::reverse(snippets.begin(), snippets.end());

  if (snippets.empty()) {
    return "Previous session had no user/assistant messages to summarize.";
  }

  std::string summary = "Recent context from previous session:\n";
  for (const auto& snippet : snippets) {
    summary += snippet + "\n";
  }
  return summary;
}

}  // namespace coding_agent

#pragma once

#include <optional>
#include <string>
#include <vector>

namespace coding_agent {

struct Config {
  std::string provider;
  std::string base_url;
  std::string model;
  std::string api_key;
  std::string cwd;
  std::optional<std::string> system_prompt_path;
  std::vector<std::string> append_system_prompts;
  std::optional<std::string> session_id;
  std::optional<std::string> prompt;
  int max_tokens;
  int context_size;
  int compaction_reserve_tokens;
  int compaction_keep_recent_tokens;
  int max_tool_iterations;
  float temperature;
  bool print_mode;
  bool stream;
  bool no_tools;
  bool no_context_files;
  bool new_session;
  bool compaction_fail_fast;
  /// When false, skip LLM branch summarization on `--new-session` / cross-session resume.
  bool branch_summary;
};

std::optional<Config> parse_config(int argc, char** argv, std::string& error);
void print_usage();

}  // namespace coding_agent

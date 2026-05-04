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
  /// Comma-separated tool names exposed to the model (must match registered tools).
  std::string initial_active_tools;
  /// Tool execution mode: "sequential" (default) or "parallel".
  std::string tool_execution_mode = "sequential";
  /// After each interactive prompt, print turn diagnostics (tail of assistant text, rounds, etc.).
  bool interactive_debug = true;
  /// When the model ends with no tools and empty text, append a nudge user message and retry (0 = off).
  int max_empty_completion_nudges = 2;
  /// Retry settings for transient LLM errors.
  bool retry_enabled = true;
  int retry_max_retries = 3;
  int retry_base_delay_ms = 1000;
  int retry_max_retry_delay_ms = 60000;
  int retry_timeout_ms = 30000;
  /// Sub-agent GPU lock file path (default: <cwd>/.pi/gpu.lock).
  std::string gpu_lock_path;
};

std::optional<Config> parse_config(int argc, char** argv, std::string& error);
void print_usage();

}  // namespace coding_agent

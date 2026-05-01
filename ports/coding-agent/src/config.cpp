#include "config.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>

#include <nlohmann/json.hpp>

namespace coding_agent {
namespace {

using nlohmann::json;

std::string get_env_or_default(const char* key, const std::string& fallback) {
  const char* value = std::getenv(key);
  return value == nullptr ? fallback : std::string(value);
}

bool parse_int_arg(const std::string& raw, int& out) {
  try {
    out = std::stoi(raw);
    return true;
  } catch (...) {
    return false;
  }
}

bool parse_float_arg(const std::string& raw, float& out) {
  try {
    out = std::stof(raw);
    return true;
  } catch (...) {
    return false;
  }
}

std::optional<json> load_settings_json() {
  const char* home = std::getenv("HOME");
  if (home == nullptr) {
    return std::nullopt;
  }

  const std::filesystem::path path =
      std::filesystem::path(home) / ".config" / "coding-agent" / "settings.json";
  if (!std::filesystem::exists(path)) {
    return std::nullopt;
  }

  std::ifstream input(path);
  if (!input.is_open()) {
    return std::nullopt;
  }

  try {
    json parsed;
    input >> parsed;
    return parsed;
  } catch (...) {
    return std::nullopt;
  }
}

template <typename T>
void load_optional(const json& source, const char* key, T& out) {
  if (source.contains(key)) {
    out = source.at(key).get<T>();
  }
}

}  // namespace

void print_usage() {
  std::cout
      << "coding-agent (C++ port, stripped-down)\n"
      << "\n"
      << "Usage:\n"
      << "  coding-agent [options] --prompt \"your prompt\"\n"
      << "\n"
      << "Options:\n"
      << "  --provider <name>         Provider name (default: llama-cpp)\n"
      << "  --base-url <url>          llama.cpp HTTP base URL\n"
      << "                            (default: http://127.0.0.1:8080)\n"
      << "  --model <id>              Model identifier passed through to provider\n"
      << "                            (default: from CODING_AGENT_MODEL or empty)\n"
      << "  --api-key <key>           Optional bearer token for llama.cpp server\n"
      << "                            (default: from CODING_AGENT_API_KEY or empty)\n"
      << "  --cwd <dir>               Working directory for tool execution\n"
      << "  --system-prompt <file>    Replace default system prompt with file content\n"
      << "  --append-system-prompt    Add extra system prompt text (repeatable)\n"
      << "  --session <id>            Resume specific session id\n"
      << "  --new-session             Force new session\n"
      << "  --context-size <int>      Context window used for compaction checks\n"
      << "  --compaction-reserve-tokens <int>\n"
      << "                            Tokens reserved before context limit (default: 16384)\n"
      << "  --compaction-keep-recent-tokens <int>\n"
      << "                            Approx recent tokens to keep verbatim (default: 20000)\n"
      << "  --max-tool-iterations <int>\n"
      << "                            Max LLM rounds per user request (default: 40)\n"
      << "  --max-tokens <int>        Maximum generated tokens (default: 512)\n"
      << "  --temperature <float>     Sampling temperature (default: 0.6)\n"
      << "  --print                   Force one-shot print mode\n"
      << "  --no-tools                Disable tool calling\n"
      << "  --no-context-files        Do not load AGENTS.md/CLAUDE.md\n"
      << "  --no-stream               Disable streaming mode\n"
      << "  --no-compaction-fail-fast\n"
      << "                            Skip compaction on failure instead of aborting\n"
      << "  --no-branch-summary       Skip branch summarization when starting a new session or switching sessions\n"
      << "  --active-tools <csv>      Comma-separated tool names for the model (default: read,bash,edit,write)\n"
      << "  --prompt <text>           Prompt text to send\n"
      << "  --help                    Show this help\n"
      << "\n"
      << "Env:\n"
      << "  CODING_AGENT_PROVIDER\n"
      << "  CODING_AGENT_BASE_URL\n"
      << "  CODING_AGENT_MODEL\n"
      << "  CODING_AGENT_API_KEY\n"
      << "  CODING_AGENT_MAX_TOOL_ITERATIONS\n"
      << "Settings file:\n"
      << "  ~/.config/coding-agent/settings.json\n";
}

std::optional<Config> parse_config(int argc, char** argv, std::string& error) {
  Config config{
      .provider = "llama-cpp",
      .base_url = "http://127.0.0.1:8080",
      .model = "",
      .api_key = "",
      .cwd = std::filesystem::current_path().string(),
      .system_prompt_path = std::nullopt,
      .append_system_prompts = {},
      .session_id = std::nullopt,
      .prompt = std::nullopt,
      .max_tokens = 512,
      .context_size = 262144,
      .compaction_reserve_tokens = 16384,
      .compaction_keep_recent_tokens = 20000,
      .max_tool_iterations = 200,
      .temperature = 0.6f,
      .print_mode = false,
      .stream = true,
      .no_tools = false,
      .no_context_files = false,
      .new_session = false,
      .compaction_fail_fast = true,
      .branch_summary = true,
      .initial_active_tools = "read,bash,edit,write",
  };

  if (const auto settings = load_settings_json(); settings.has_value()) {
    load_optional(settings.value(), "base_url", config.base_url);
    load_optional(settings.value(), "model", config.model);
    load_optional(settings.value(), "api_key", config.api_key);
    load_optional(settings.value(), "temperature", config.temperature);
    load_optional(settings.value(), "max_tokens", config.max_tokens);
    load_optional(settings.value(), "context_size", config.context_size);
    load_optional(settings.value(), "compaction_reserve_tokens", config.compaction_reserve_tokens);
    load_optional(settings.value(), "compaction_keep_recent_tokens", config.compaction_keep_recent_tokens);
    load_optional(settings.value(), "compaction_fail_fast", config.compaction_fail_fast);
    load_optional(settings.value(), "initial_active_tools", config.initial_active_tools);
  }

  config.provider = get_env_or_default("CODING_AGENT_PROVIDER", config.provider);
  config.base_url = get_env_or_default("CODING_AGENT_BASE_URL", config.base_url);
  config.model = get_env_or_default("CODING_AGENT_MODEL", config.model);
  config.api_key = get_env_or_default("CODING_AGENT_API_KEY", config.api_key);
  const char* max_iter_env = std::getenv("CODING_AGENT_MAX_TOOL_ITERATIONS");
  if (max_iter_env != nullptr) {
    try {
      config.max_tool_iterations = std::stoi(max_iter_env);
    } catch (...) {
      error = "Invalid value for CODING_AGENT_MAX_TOOL_ITERATIONS";
      return std::nullopt;
    }
  }

  if (config.provider.empty()) {
    config.provider = "llama-cpp";
  }
  if (config.base_url.empty()) {
    config.base_url = "http://127.0.0.1:8080";
  }

  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];

    if (arg == "--help") {
      print_usage();
      return std::nullopt;
    }
    if (arg == "--provider" && i + 1 < argc) {
      config.provider = argv[++i];
      continue;
    }
    if (arg == "--base-url" && i + 1 < argc) {
      config.base_url = argv[++i];
      continue;
    }
    if (arg == "--model" && i + 1 < argc) {
      config.model = argv[++i];
      continue;
    }
    if (arg == "--api-key" && i + 1 < argc) {
      config.api_key = argv[++i];
      continue;
    }
    if (arg == "--cwd" && i + 1 < argc) {
      config.cwd = argv[++i];
      continue;
    }
    if (arg == "--system-prompt" && i + 1 < argc) {
      config.system_prompt_path = std::string(argv[++i]);
      continue;
    }
    if (arg == "--append-system-prompt" && i + 1 < argc) {
      config.append_system_prompts.push_back(argv[++i]);
      continue;
    }
    if (arg == "--session" && i + 1 < argc) {
      config.session_id = std::string(argv[++i]);
      continue;
    }
    if (arg == "--new-session") {
      config.new_session = true;
      continue;
    }
    if (arg == "--context-size" && i + 1 < argc) {
      if (!parse_int_arg(argv[++i], config.context_size)) {
        error = "Invalid value for --context-size";
        return std::nullopt;
      }
      continue;
    }
    if (arg == "--compaction-reserve-tokens" && i + 1 < argc) {
      if (!parse_int_arg(argv[++i], config.compaction_reserve_tokens)) {
        error = "Invalid value for --compaction-reserve-tokens";
        return std::nullopt;
      }
      continue;
    }
    if (arg == "--compaction-keep-recent-tokens" && i + 1 < argc) {
      if (!parse_int_arg(argv[++i], config.compaction_keep_recent_tokens)) {
        error = "Invalid value for --compaction-keep-recent-tokens";
        return std::nullopt;
      }
      continue;
    }
    if (arg == "--max-tool-iterations" && i + 1 < argc) {
      if (!parse_int_arg(argv[++i], config.max_tool_iterations)) {
        error = "Invalid value for --max-tool-iterations";
        return std::nullopt;
      }
      continue;
    }
    if ((arg == "--max-tokens" || arg == "--n-predict") && i + 1 < argc) {
      if (!parse_int_arg(argv[++i], config.max_tokens)) {
        error = "Invalid value for --max-tokens";
        return std::nullopt;
      }
      continue;
    }
    if (arg == "--temperature" && i + 1 < argc) {
      if (!parse_float_arg(argv[++i], config.temperature)) {
        error = "Invalid value for --temperature";
        return std::nullopt;
      }
      continue;
    }
    if (arg == "--no-stream") {
      config.stream = false;
      continue;
    }
    if (arg == "--print") {
      config.print_mode = true;
      continue;
    }
    if (arg == "--no-tools") {
      config.no_tools = true;
      continue;
    }
    if (arg == "--no-context-files") {
      config.no_context_files = true;
      continue;
    }
    if (arg == "--no-compaction-fail-fast") {
      config.compaction_fail_fast = false;
      continue;
    }
    if (arg == "--no-branch-summary") {
      config.branch_summary = false;
      continue;
    }
    if (arg == "--active-tools" && i + 1 < argc) {
      config.initial_active_tools = argv[++i];
      continue;
    }
    if (arg == "--prompt" && i + 1 < argc) {
      config.prompt = std::string(argv[++i]);
      continue;
    }

    if (arg.rfind("--", 0) == 0) {
      error = "Unknown or incomplete argument: " + arg;
      return std::nullopt;
    }
  }

  if (config.compaction_reserve_tokens < 0) {
    error = "--compaction-reserve-tokens must be >= 0";
    return std::nullopt;
  }
  if (config.compaction_keep_recent_tokens <= 0) {
    error = "--compaction-keep-recent-tokens must be > 0";
    return std::nullopt;
  }

  return config;
}

}  // namespace coding_agent

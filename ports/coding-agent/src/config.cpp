#include "config.hpp"

#include <cstdlib>
#include <iostream>
#include <string>

namespace coding_agent {
namespace {

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
      << "  --n-predict <int>         Maximum generated tokens (default: 512)\n"
      << "  --temperature <float>     Sampling temperature (default: 0.2)\n"
      << "  --no-stream               Disable streaming mode\n"
      << "  --prompt <text>           Prompt text to send\n"
      << "  --help                    Show this help\n"
      << "\n"
      << "Env:\n"
      << "  CODING_AGENT_PROVIDER\n"
      << "  CODING_AGENT_BASE_URL\n"
      << "  CODING_AGENT_MODEL\n"
      << "  CODING_AGENT_API_KEY\n";
}

std::optional<Config> parse_config(int argc, char** argv, std::string& error) {
  Config config{
      .provider = get_env_or_default("CODING_AGENT_PROVIDER", "llama-cpp"),
      .base_url = get_env_or_default("CODING_AGENT_BASE_URL", "http://127.0.0.1:8080"),
      .model = get_env_or_default("CODING_AGENT_MODEL", ""),
      .api_key = get_env_or_default("CODING_AGENT_API_KEY", ""),
      .n_predict = 512,
      .temperature = 0.2f,
      .stream = true,
  };

  bool has_prompt = false;

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
    if (arg == "--n-predict" && i + 1 < argc) {
      if (!parse_int_arg(argv[++i], config.n_predict)) {
        error = "Invalid value for --n-predict";
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
    if (arg == "--prompt" && i + 1 < argc) {
      has_prompt = true;
      ++i;
      continue;
    }

    if (arg.rfind("--", 0) == 0) {
      error = "Unknown or incomplete argument: " + arg;
      return std::nullopt;
    }
  }

  if (!has_prompt) {
    error = "Missing required argument: --prompt";
    return std::nullopt;
  }

  return config;
}

}  // namespace coding_agent

#include "agent.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include <unistd.h>

#include "context_loader.hpp"
#include "modes/interactive_mode.hpp"
#include "modes/print_mode.hpp"
#include "providers/llama_cpp_provider.hpp"
#include "session.hpp"
#include "system_prompt.hpp"
#include "tools/tool_registry.hpp"
#include "config.hpp"

namespace coding_agent {
namespace {

std::string read_file(const std::string& path) {
  std::ifstream input(path);
  std::stringstream buffer;
  buffer << input.rdbuf();
  return buffer.str();
}

bool should_run_print_mode(const Config& config) {
  if (config.print_mode || config.prompt.has_value()) {
    return true;
  }
  return !isatty(STDIN_FILENO);
}

}  // namespace

int run_agent(int argc, char** argv) {
  std::string parse_error;
  auto config = parse_config(argc, argv, parse_error);
  if (!config.has_value()) {
    if (!parse_error.empty()) {
      std::cerr << "Error: " << parse_error << "\n\n";
      print_usage();
      return 1;
    }
    return 0;
  }

  if (config->provider != "llama-cpp") {
    std::cerr << "Error: unsupported provider '" << config->provider
              << "'. This port currently supports only llama-cpp.\n";
    return 1;
  }

  if (!std::filesystem::exists(config->cwd) || !std::filesystem::is_directory(config->cwd)) {
    std::cerr << "Error: cwd is not a valid directory: " << config->cwd << "\n";
    return 1;
  }

  std::filesystem::current_path(config->cwd);

  LlamaCppProvider provider(config->base_url, config->api_key);
  ToolRegistry tools;
  if (!config->no_tools) {
    register_builtin_tools(tools);
  }

  SessionStore session(config->cwd);
  session.start_or_resume(config->session_id, config->new_session);

  std::string load_error;
  std::vector<ChatMessage> history = session.load_messages(load_error);

  if (history.empty() || history.front().role != "system") {
    std::vector<ContextFile> context_files;
    if (!config->no_context_files) {
      context_files = load_context_files(config->cwd);
    }
    std::string system_prompt = build_system_prompt(
        config->cwd,
        config->no_tools ? std::vector<ToolDefinition>{} : tools.build_tool_definitions(),
        context_files,
        config->append_system_prompts
    );
    if (config->system_prompt_path.has_value()) {
      system_prompt = read_file(config->system_prompt_path.value());
    }
    ChatMessage system{
        .role = "system",
        .content = system_prompt,
        .tool_call_id = std::nullopt,
        .tool_calls = {},
    };
    history.insert(history.begin(), system);
    std::string persist_error;
    session.append(system, persist_error);
  }

  if (should_run_print_mode(config.value())) {
    return run_print_mode(config.value(), provider, tools, history, session);
  }
  return run_interactive_mode(config.value(), provider, tools, history, session);
}

}  // namespace coding_agent

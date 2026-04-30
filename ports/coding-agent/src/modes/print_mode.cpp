#include "modes/print_mode.hpp"

#include <iostream>
#include <sstream>

namespace coding_agent {
namespace {

std::string read_stdin_if_any() {
  std::stringstream buffer;
  buffer << std::cin.rdbuf();
  return buffer.str();
}

}  // namespace

int run_print_mode(
    const Config& config,
    Provider& provider,
    ToolRegistry& tools,
    std::vector<ChatMessage>& history,
    SessionStore& session
) {
  std::string prompt = config.prompt.value_or("");
  if (prompt.empty()) {
    prompt = read_stdin_if_any();
  }
  if (prompt.empty()) {
    std::cerr << "Error: missing prompt (--prompt or piped stdin)\n";
    return 1;
  }

  const RunResult result =
      run_agent_loop(config, provider, tools, history, session, prompt, [](const std::string& chunk) {
        std::cout << chunk << std::flush;
      });
  if (!result.ok) {
    std::cerr << "\nError: " << result.error << "\n";
    return 1;
  }
  std::cout << "\n";
  return 0;
}

}  // namespace coding_agent

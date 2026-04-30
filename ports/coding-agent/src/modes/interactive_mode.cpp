#include "modes/interactive_mode.hpp"

#include <cstdlib>
#include <iostream>
#include <string>

#include <readline/history.h>
#include <readline/readline.h>

#include "agent_loop.hpp"

namespace coding_agent {

int run_interactive_mode(
    const Config& config,
    Provider& provider,
    ToolRegistry& tools,
    std::vector<ChatMessage>& history,
    SessionStore& session
) {
  while (true) {
    char* line = readline("> ");
    if (line == nullptr) {
      std::cout << "\n";
      return 0;
    }

    std::string prompt(line);
    free(line);
    if (prompt.empty()) {
      continue;
    }
    if (prompt == "/exit" || prompt == "/quit") {
      return 0;
    }
    if (prompt == "/clear") {
      history.clear();
      std::cout << "History cleared.\n";
      continue;
    }

    add_history(prompt.c_str());
    const RunResult result =
        run_agent_loop(config, provider, tools, history, session, prompt, [](const std::string& chunk) {
          std::cout << chunk << std::flush;
        });
    if (!result.ok) {
      std::cerr << "\nError: " << result.error << "\n";
    } else {
      std::cout << "\n";
    }
  }
}

}  // namespace coding_agent

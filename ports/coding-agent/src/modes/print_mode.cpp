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

int run_print_mode(AgentSession& agent, const std::string& prompt) {
    std::string input = prompt;
    if (input.empty()) {
        input = read_stdin_if_any();
    }
    if (input.empty()) {
        std::cerr << "Error: missing prompt (--prompt or piped stdin)\n";
        return 1;
    }

    const bool ok = agent.run(input, [](const std::string& chunk) {
        std::cout << chunk << std::flush;
    });

    if (!ok) {
        std::cerr << "\nError: agent run failed\n";
        return 1;
    }
    std::cout << "\n";
    return 0;
}

}  // namespace coding_agent

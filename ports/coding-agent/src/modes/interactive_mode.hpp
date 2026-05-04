#pragma once

#include <optional>
#include <string>
#include <vector>

#include "agent_session.hpp"

namespace coding_agent {

int run_interactive_mode(AgentSession& agent, bool interactive_debug,
                         int argc, char** argv);

}  // namespace coding_agent

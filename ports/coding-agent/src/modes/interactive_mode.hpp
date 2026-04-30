#pragma once

#include "config.hpp"
#include "providers/provider.hpp"
#include "session.hpp"
#include "tools/tool_registry.hpp"

namespace coding_agent {

int run_interactive_mode(
    const Config& config,
    Provider& provider,
    ToolRegistry& tools,
    std::vector<ChatMessage>& history,
    SessionStore& session
);

}  // namespace coding_agent

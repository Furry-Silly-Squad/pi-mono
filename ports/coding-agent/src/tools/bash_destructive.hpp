#pragma once

#include <string>

namespace coding_agent {

/// Heuristic matching for commands that should require explicit user confirmation
/// before execution (mirrors the removed in-tool guardrail; used by AgentSession).
bool bash_command_looks_destructive(const std::string& command);

}  // namespace coding_agent

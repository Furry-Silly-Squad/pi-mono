#pragma once

#include <atomic>
#include <string>
#include <vector>

#include "compaction.hpp"
#include "config.hpp"
#include "providers/provider.hpp"
#include "session.hpp"
#include "tools/tool_registry.hpp"

namespace coding_agent {

struct RunResult {
  bool ok;
  std::string output;
  std::string error;
};

RunResult run_agent_loop(
    const Config& config,
    Provider& provider,
    ToolRegistry& tools,
    std::vector<ChatMessage>& history,
    SessionStore& session,
    const std::string& user_input,
    const ChunkCallback& on_chunk,
    std::atomic<bool>* cancel_flag = nullptr
);

}  // namespace coding_agent

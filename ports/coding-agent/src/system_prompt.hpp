#pragma once

#include <string>
#include <vector>

#include "context_loader.hpp"
#include "providers/provider.hpp"

namespace coding_agent {

std::string build_system_prompt(
    const std::string& cwd,
    const std::vector<ToolDefinition>& tools,
    const std::vector<ContextFile>& context_files,
    const std::vector<std::string>& append_prompts
);

}  // namespace coding_agent

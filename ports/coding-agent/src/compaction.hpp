#pragma once

#include <string>
#include <vector>

#include "providers/provider.hpp"

namespace coding_agent {

int approx_tokens(const std::string& text);
int total_context_tokens(const std::vector<ChatMessage>& messages);
bool should_compact(int total_tokens, int context_size);
bool compact_history(
    std::vector<ChatMessage>& messages,
    Provider& provider,
    const std::string& model,
    std::string& error
);

}  // namespace coding_agent

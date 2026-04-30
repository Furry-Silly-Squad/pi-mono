#pragma once

#include <string>

#include "providers/provider.hpp"

namespace coding_agent {

class LlamaCppProvider final : public Provider {
 public:
  explicit LlamaCppProvider(std::string base_url, std::string api_key);
  bool chat(
      const ChatRequest& request,
      ChatResponse& response,
      const ChunkCallback& on_chunk,
      std::string& error
  ) override;

 private:
  std::string base_url_;
  std::string api_key_;
};

}  // namespace coding_agent

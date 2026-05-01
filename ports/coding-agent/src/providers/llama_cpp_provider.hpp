#pragma once

#include <atomic>
#include <string>

#include <curl/curl.h>

#include "providers/provider.hpp"

namespace coding_agent {

class LlamaCppProvider final : public Provider {
 public:
  explicit LlamaCppProvider(std::string base_url, std::string api_key);
  bool chat(
      const ChatRequest& request,
      ChatResponse& response,
      const ChunkCallback& on_chunk,
      std::string& error,
      std::atomic<bool>* cancel_flag = nullptr
  ) override;
  void cancel() override;

 private:
  std::string base_url_;
  std::string api_key_;
  std::atomic<bool> interrupted_ = false;
  CURL* active_curl_ = nullptr;
};

}  // namespace coding_agent

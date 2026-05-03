#pragma once

#include <atomic>
#include <cstdint>
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
  void set_api_key(const std::string& key) override {
    api_key_ = key;
  }

 private:
  std::string base_url_;
  std::string api_key_;
  std::atomic<bool> interrupted_ = false;
  CURL* active_curl_ = nullptr;
  std::atomic<bool> fallback_cancel_{false};
  // Atomic address of the active cancel flag to avoid data races on the
  // pointer itself.  cancel() stores via release, chat() loads via acquire.
  std::atomic<std::uintptr_t> active_cancel_flag_addr_{0};
};

}  // namespace coding_agent

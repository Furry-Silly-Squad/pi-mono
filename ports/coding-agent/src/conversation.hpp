#pragma once

#include <string>
#include <vector>

#include "providers/provider.hpp"

namespace coding_agent {

class Conversation {
 public:
  void clear();
  void append(const ChatMessage& message);
  const std::vector<ChatMessage>& messages() const;

 private:
  std::vector<ChatMessage> messages_;
};

}  // namespace coding_agent

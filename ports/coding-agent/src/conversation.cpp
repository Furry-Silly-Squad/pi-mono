#include "conversation.hpp"

namespace coding_agent {

void Conversation::clear() {
  messages_.clear();
}

void Conversation::append(const ChatMessage& message) {
  messages_.push_back(message);
}

const std::vector<ChatMessage>& Conversation::messages() const {
  return messages_;
}

}  // namespace coding_agent

#pragma once

#include <atomic>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace coding_agent {

struct ToolCall {
  std::string id;
  std::string name;
  std::string arguments_json;
};

struct ChatMessage {
  std::string role;
  std::string content;
  std::optional<std::string> tool_call_id = {};
  std::vector<ToolCall> tool_calls = {};
  std::optional<std::string> entry_id = {};
  int usage_tokens = 0;
};

struct ToolDefinition {
  std::string name;
  std::string description;
  std::string parameters_schema_json;
};

struct ChatRequest {
  std::vector<ChatMessage> messages;
  std::vector<ToolDefinition> tools;
  std::string model;
  int max_tokens;
  float temperature;
  bool stream;
};

struct ChatResponse {
  std::string content;
  std::vector<ToolCall> tool_calls;
  int prompt_tokens = 0;
  int completion_tokens = 0;
};

using ChunkCallback = std::function<void(const std::string&)>;

class Provider {
 public:
  virtual ~Provider() = default;
  virtual bool chat(
      const ChatRequest& request,
      ChatResponse& response,
      const ChunkCallback& on_chunk,
      std::string& error,
      std::atomic<bool>* cancel_flag = nullptr
  ) = 0;
  virtual void cancel() = 0;
};

}  // namespace coding_agent

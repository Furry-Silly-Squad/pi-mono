#pragma once

#include <string>

namespace coding_agent {

enum class ToolExecutionMode : uint8_t {
  Sequential,
  Parallel,
};

struct ToolResult {
  bool ok;
  std::string content;
};

class Tool {
 public:
  virtual ~Tool() = default;
  virtual std::string name() const = 0;
  virtual std::string description() const = 0;
  virtual std::string parameters_schema() const = 0;
  virtual ToolExecutionMode execution_mode() const { return ToolExecutionMode::Parallel; }
  virtual ToolResult execute(const std::string& args_json, const std::string& cwd) = 0;
};

}  // namespace coding_agent

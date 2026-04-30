#pragma once

#include <string>

namespace coding_agent {

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
  virtual ToolResult execute(const std::string& args_json, const std::string& cwd) = 0;
};

}  // namespace coding_agent

#pragma once

#include "tools/tool.hpp"

namespace coding_agent {

class ReadTool final : public Tool {
 public:
  std::string name() const override;
  std::string description() const override;
  std::string parameters_schema() const override;
  ToolResult execute(const std::string& args_json, const std::string& cwd) override;
};

}  // namespace coding_agent

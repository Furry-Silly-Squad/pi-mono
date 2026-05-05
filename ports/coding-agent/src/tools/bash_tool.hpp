#pragma once

#include <atomic>
#include <string>

#include "tools/tool.hpp"

namespace coding_agent {

class BashTool final : public Tool {
 public:
  std::string name() const override;
  std::string description() const override;
  std::string parameters_schema() const override;
  ToolExecutionMode execution_mode() const override;

  /// Set the atomic flag that the tool checks during execution.
  /// Called by AgentSession::set_bash_cancel_flag().
  void set_cancel_flag(std::atomic<bool>* flag);

  /// Clear the cancel flag reference.
  void clear_cancel_flag();

  ToolResult execute(const std::string& args_json, const std::string& cwd) override;

 private:
  std::atomic<bool>* cancel_flag_ = nullptr;
};

}  // namespace coding_agent

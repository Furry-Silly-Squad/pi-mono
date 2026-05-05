#pragma once

#include <memory>
#include <string>
#include <vector>

#include "providers/provider.hpp"
#include "tools/tool.hpp"

namespace coding_agent {

class ToolRegistry {
 public:
  void register_tool(std::unique_ptr<Tool> tool);
  std::vector<ToolDefinition> build_tool_definitions() const;
  ToolResult dispatch(const std::string& name, const std::string& args_json, const std::string& cwd) const;

  /// Set the atomic flag that BashTool checks during execution.
  void set_bash_cancel_flag(std::atomic<bool>* flag);

 private:
  std::vector<std::unique_ptr<Tool>> tools_;
};

void register_builtin_tools(ToolRegistry& registry);

}  // namespace coding_agent

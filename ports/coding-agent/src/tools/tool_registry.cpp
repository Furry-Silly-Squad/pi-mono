#include "tools/tool_registry.hpp"

#include <nlohmann/json.hpp>

#include "tools/bash_tool.hpp"
#include "tools/edit_tool.hpp"
#include "tools/find_tool.hpp"
#include "tools/grep_tool.hpp"
#include "tools/ls_tool.hpp"
#include "tools/read_tool.hpp"
#include "tools/write_tool.hpp"

namespace coding_agent {

void ToolRegistry::register_tool(std::unique_ptr<Tool> tool) {
  tools_.push_back(std::move(tool));
}

std::vector<ToolDefinition> ToolRegistry::build_tool_definitions() const {
  std::vector<ToolDefinition> definitions;
  for (const auto& tool : tools_) {
    definitions.push_back(
        ToolDefinition{
            .name = tool->name(),
            .description = tool->description(),
            .parameters_schema_json = tool->parameters_schema(),
            .execution_mode = tool->execution_mode(),
        }
    );
  }
  return definitions;
}

ToolResult ToolRegistry::dispatch(const std::string& name, const std::string& args_json, const std::string& cwd)
    const {
  for (const auto& tool : tools_) {
    if (tool->name() == name) {
      return tool->execute(args_json, cwd);
    }
  }
  return ToolResult{
      .ok = false,
      .content = "Unknown tool: " + name,
  };
}

void ToolRegistry::set_bash_cancel_flag(std::atomic<bool>* flag) {
  for (auto& tool : tools_) {
    if (auto* bash = dynamic_cast<BashTool*>(tool.get())) {
      bash->set_cancel_flag(flag);
    }
  }
}

void register_builtin_tools(ToolRegistry& registry) {
  registry.register_tool(std::make_unique<ReadTool>());
  registry.register_tool(std::make_unique<WriteTool>());
  registry.register_tool(std::make_unique<EditTool>());
  registry.register_tool(std::make_unique<BashTool>());
  registry.register_tool(std::make_unique<GrepTool>());
  registry.register_tool(std::make_unique<FindTool>());
  registry.register_tool(std::make_unique<LsTool>());
}

}  // namespace coding_agent

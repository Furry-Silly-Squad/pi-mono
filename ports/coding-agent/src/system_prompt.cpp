#include "system_prompt.hpp"

#include <chrono>
#include <sstream>

namespace coding_agent {
namespace {

std::string iso_timestamp() {
  const auto now = std::chrono::system_clock::now();
  const auto seconds = std::chrono::time_point_cast<std::chrono::seconds>(now);
  return std::to_string(seconds.time_since_epoch().count());
}

}  // namespace

std::string build_system_prompt(
    const std::string& cwd,
    const std::vector<ToolDefinition>& tools,
    const std::vector<ContextFile>& context_files,
    const std::vector<std::string>& append_prompts
) {
  std::ostringstream out;
  out << "You are pi coding-agent (C++ port).\n";
  out << "Current working directory: " << cwd << "\n";
  out << "Current timestamp: " << iso_timestamp() << "\n";
  out << "Prefer concise, precise technical output.\n";
  out << "Use tools when needed and explain important actions.\n";

  if (!tools.empty()) {
    out << "\nAvailable tools:\n";
    for (const auto& tool : tools) {
      out << "- " << tool.name << ": " << tool.description << "\n";
    }
  }

  if (!context_files.empty()) {
    out << "\nProject context files:\n";
    for (const auto& file : context_files) {
      out << "## " << file.path << "\n" << file.content << "\n";
    }
  }

  if (!append_prompts.empty()) {
    out << "\nAdditional system instructions:\n";
    for (const auto& p : append_prompts) {
      out << p << "\n";
    }
  }

  return out.str();
}

}  // namespace coding_agent

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
  out << "When you are done calling tools for this step, always write a short user-visible "
         "message: what you did, what you found, and next steps. Never finish a turn with an "
         "empty assistant message unless you are immediately issuing tool calls.\n";

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

  out << "\n## Task Delegation (Phase 11)\n\n";
  out << "If the user's request involves multiple distinct tasks (e.g., "
      << "\"implement X, add tests, update docs\"),\n";
  out << "you MUST decompose it into subtasks before executing. Respond with a JSON object "
      << "matching this schema:\n\n";
  out << "{\n";
  out << "  \"decomposition\": {\n";
  out << "    \"description\": \"Brief summary of the user's request\",\n";
  out << "    \"subtasks\": [\n";
  out << "      {\n";
  out << "        \"id\": \"1\",\n";
  out << "        \"description\": \"Natural language description of what to do\",\n";
  out << "        \"context_files\": [\"file1\", \"file2\"],\n";
  out << "        \"expected_artifacts\": [\"output1\", \"output2\"],\n";
  out << "        \"dependencies\": [],\n";
  out << "        \"priority\": 1\n";
  out << "      }\n";
  out << "    ]\n";
  out << "  }\n";
  out << "}\n\n";
  out << "Rules:\n";
  out << "- Each subtask should be self-contained and executable by a single coding-agent "
      << "process\n";
  out << "- Dependencies must form a DAG (no cycles)\n";
  out << "- Priority is used for execution order (lower number = execute first)\n";
  out << "- Context files are files the sub-agent should read before starting\n";
  out << "- Expected artifacts are files the sub-agent is expected to create or modify\n";
  out << "- If the request is a single task, respond with a single subtask\n";
  out << "- If the request doesn't need decomposition, respond with a single subtask that "
      << "handles the whole request\n";

  return out.str();
}

}  // namespace coding_agent

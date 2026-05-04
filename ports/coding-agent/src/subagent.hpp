#pragma once

#include <string>
#include <vector>
#include <optional>

#include "server_config.hpp"

namespace coding_agent {

/// Result from a spawned sub-agent process.
struct SubAgentResult {
  std::string sessionId;        // Child session ID
  std::string sessionPath;      // Full path to child session file
  std::string lastAssistantMessage;  // Last assistant message content
  bool success = false;
  std::optional<std::string> error;
  int exitCode = -1;
};

/// A single subtask with all fields from the LLM decomposition.
struct SubTask {
    std::string id;
    std::string description;
    std::vector<std::string> contextFiles;
    std::vector<std::string> expectedArtifacts;
    std::vector<std::string> dependencies;  // IDs of prerequisite subtasks
    int priority = 0;  // lower = higher priority
    std::string server;  // Optional server ID for routing (empty = use parent's server)
};

/// Represents the parsed result of a decomposition JSON response.
struct DecompositionResult {
    std::string description;
    std::vector<SubTask> subtasks;
};

/// Validate that the subtask dependency graph is a valid DAG (no cycles).
/// Returns nullopt if valid, or an error message describing the issue.
std::optional<std::string> validateSubtaskDag(const std::vector<SubTask>& subtasks);

/// Compute a topological execution order for the subtasks based on their dependencies.
/// Returns an ordered list of subtask IDs. Returns nullopt if the graph is invalid.
std::optional<std::vector<std::string>> topologicalSortSubtasks(const std::vector<SubTask>& subtasks);

/// Parse a decomposition JSON response from the LLM.
/// Handles both nested `{ "decomposition": { "description", "subtasks" } }`
/// and flat `{ "description", "subtasks" }` formats.
///
/// @param jsonStr The raw JSON string from the LLM response.
/// @param error On failure, populated with a human-readable error message.
/// @return DecompositionResult on success, std::nullopt on failure.
std::optional<DecompositionResult> parseDecompositionJson(
    const std::string& jsonStr,
    std::string& error
);

/// Spawned sub-agent process manager.
class SubAgent {
 public:
  /// Spawn a child coding-agent process for a subtask.
  /// Blocks until the child completes or the timeout expires.
  ///
  /// @param binaryPath Path to the coding-agent binary.
  /// @param subtaskDescription Natural language description of the subtask.
  /// @param contextFiles Files the sub-agent should read before starting.
  /// @param serverConfig Server connectivity configuration.
  /// @param parentSessionDir Parent session directory for child session storage.
  /// @param gpuLockPath Path to GPU semaphore lock file.
  /// @param maxTokens Maximum tokens for the child process.
  /// @param temperature Temperature for the child process.
  /// @param maxSubtaskDurationMs Maximum allowed duration in milliseconds (default 30 min).
  /// @return SubAgentResult with session info and exit status.
  static SubAgentResult spawn(
      const std::string& binaryPath,
      const std::string& subtaskDescription,
      const std::vector<std::string>& contextFiles,
      const ServerConfig& serverConfig,
      const std::string& parentSessionDir,
      const std::string& gpuLockPath,
      int maxTokens,
      float temperature,
      int maxSubtaskDurationMs = 1800000
  );

  /// Read the last assistant message from a session file.
  ///
  /// @param sessionPath Path to the session file.
  /// @return Last assistant message content, or empty string if none.
  static std::string readLastAssistantMessage(const std::string& sessionPath);
};

}  // namespace coding_agent

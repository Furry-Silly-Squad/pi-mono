#pragma once

#include <string>
#include <vector>
#include <optional>

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

/// Server connectivity configuration for sub-agent processes.
struct ServerConfig {
  std::string baseUrl;       // e.g. "http://127.0.0.1:8080"
  std::string modelId;       // model used by parent
  std::string apiKey;        // if required
  std::vector<std::string> contextFiles;
};

/// Spawned sub-agent process manager.
class SubAgent {
 public:
  /// Spawn a child coding-agent process for a subtask.
  /// Blocks until the child completes.
  ///
  /// @param binaryPath Path to the coding-agent binary.
  /// @param subtaskDescription Natural language description of the subtask.
  /// @param contextFiles Files the sub-agent should read before starting.
  /// @param serverConfig Server connectivity configuration.
  /// @param parentSessionDir Parent session directory for child session storage.
  /// @param gpuLockPath Path to GPU semaphore lock file.
  /// @param maxTokens Maximum tokens for the child process.
  /// @param temperature Temperature for the child process.
  /// @return SubAgentResult with session info and exit status.
  static SubAgentResult spawn(
      const std::string& binaryPath,
      const std::string& subtaskDescription,
      const std::vector<std::string>& contextFiles,
      const ServerConfig& serverConfig,
      const std::string& parentSessionDir,
      const std::string& gpuLockPath,
      int maxTokens,
      float temperature
  );

  /// Read the last assistant message from a session file.
  ///
  /// @param sessionPath Path to the session file.
  /// @return Last assistant message content, or empty string if none.
  static std::string readLastAssistantMessage(const std::string& sessionPath);
};

}  // namespace coding_agent

#include "subagent.hpp"

#include <algorithm>
#include <chrono>
#include <csignal>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "gpu_semaphore.hpp"
#include <nlohmann/json.hpp>

#if defined(_WIN32) || defined(_WIN64)
#include <process.h>
#include <windows.h>
#else
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace coding_agent {

namespace fs = std::filesystem;

SubAgentResult SubAgent::spawn(
    const std::string& binaryPath,
    const std::string& subtaskDescription,
    const std::vector<std::string>& contextFiles,
    const ServerConfig& serverConfig,
    const std::string& parentSessionDir,
    const std::string& gpuLockPath,
    int maxTokens,
    float temperature,
    int maxSubtaskDurationMs
) {
  SubAgentResult result;
  result.sessionPath = "";
  result.lastAssistantMessage = "";
  result.success = false;
  result.exitCode = -1;

  // Generate child session ID
  const auto now = std::chrono::system_clock::now();
  const auto ms = std::chrono::time_point_cast<std::chrono::milliseconds>(now);
  const std::string timestampPrefix = std::to_string(ms.time_since_epoch().count());

  // Simple ID generation
  std::string childSessionId = timestampPrefix + "_";
  for (int i = 0; i < 8; ++i) {
    childSessionId += "0123456789abcdef"[rand() % 16];
  }

  // Create child session file path
  const fs::path childSessionPath = fs::path(parentSessionDir) /
      ("subtask-" + childSessionId + ".jsonl");
  result.sessionPath = childSessionPath.string();
  result.sessionId = childSessionId;

  // Build command line arguments
  std::vector<std::string> args;
  args.push_back(binaryPath);
  args.push_back("--provider");
  args.push_back("llama-cpp");
  args.push_back("--base-url");
  args.push_back(serverConfig.baseUrl);
  args.push_back("--model");
  args.push_back(serverConfig.modelId);

  if (!serverConfig.apiKey.empty()) {
    args.push_back("--api-key");
    args.push_back(serverConfig.apiKey);
  }

  // Add context files if any
  for (const auto& file : contextFiles) {
    args.push_back("--context-file");
    args.push_back(file);
  }

  args.push_back("--cwd");
  args.push_back(".");  // Use current working directory

  args.push_back("--new-session");
  args.push_back("--session");
  args.push_back(childSessionPath.string());

  if (maxTokens > 0) {
    args.push_back("--max-tokens");
    args.push_back(std::to_string(maxTokens));
  }

  if (temperature > 0.0f) {
    args.push_back("--temperature");
    args.push_back(std::to_string(temperature));
  }

  args.push_back("--no-branch-summary");

  // Add subtask description as the prompt
  args.push_back(subtaskDescription);

  // Acquire GPU lock before spawning
  if (!GpuSemaphore::tryAcquire(gpuLockPath, 120000)) {
    result.error = "Failed to acquire GPU lock: timeout waiting for GPU resource";
    return result;
  }

  // Spawn child process
#if defined(_WIN32) || defined(_WIN64)
  // Windows implementation
  STARTUPINFOA si = {};
  si.cb = sizeof(si);
  PROCESS_INFORMATION pi = {};

  // Build command line string
  std::string cmdLine;
  for (size_t i = 0; i < args.size(); ++i) {
    if (i > 0) cmdLine += " ";
    // Quote arguments that contain spaces
    if (args[i].find(' ') != std::string::npos) {
      cmdLine += "\"";
      cmdLine += args[i];
      cmdLine += "\"";
    } else {
      cmdLine += args[i];
    }
  }

  if (!CreateProcessA(
      nullptr,           // Application name
      &cmdLine[0],       // Command line
      nullptr,           // Process security attributes
      nullptr,           // Thread security attributes
      FALSE,             // Inherit handles
      0,                 // Creation flags
      nullptr,           // Environment
      nullptr,           // Current directory
      &si,               // Startup info
      &pi                // Process information
  )) {
    GpuSemaphore::release(gpuLockPath);
    result.error = "Failed to create child process: " + std::to_string(GetLastError());
    return result;
  }

  // Wait for child to complete with timeout
  auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(maxSubtaskDurationMs);
  bool timedOut = false;

  while (true) {
    DWORD ret = WaitForSingleObject(pi.hProcess, 500);
    if (ret == WAIT_OBJECT_0) {
      break;  // Child exited normally
    }
    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline) {
      timedOut = true;
      // Kill the child process
      TerminateProcess(pi.hProcess, 1);
      break;
    }
  }

  DWORD exitCode;
  GetExitCodeProcess(pi.hProcess, &exitCode);
  result.exitCode = static_cast<int>(exitCode);

  CloseHandle(pi.hProcess);
  CloseHandle(pi.hThread);
#else
  // Unix implementation
  pid_t pid = fork();

  if (pid == -1) {
    GpuSemaphore::release(gpuLockPath);
    result.error = "Failed to fork child process";
    return result;
  }

  if (pid == 0) {
    // Child process
    // Convert args to C-style array
    std::vector<char*> argv;
    for (const auto& arg : args) {
      argv.push_back(const_cast<char*>(arg.c_str()));
    }
    argv.push_back(nullptr);

    // Execute coding-agent binary
    execv(binaryPath.c_str(), argv.data());

    // If execv returns, it failed
    _exit(127);
  }

  // Parent process: wait for child with timeout
  auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(maxSubtaskDurationMs);
  int status = 0;
  bool timedOut = false;

  while (true) {
    int ret = waitpid(pid, &status, WNOHANG);
    if (ret == pid) {
      break;  // Child exited normally
    }
    if (ret == -1) {
      // Error (e.g., ECHILD if child already reaped)
      break;
    }
    // ret == 0: child still running
    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline) {
      timedOut = true;
      // Kill the child process group to avoid zombies
      kill(-pid, SIGKILL);
      // Reap the zombie
      waitpid(pid, &status, 0);
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
  }

  if (WIFEXITED(status)) {
    result.exitCode = WEXITSTATUS(status);
  } else if (WIFSIGNALED(status)) {
    result.exitCode = -1;
    result.error = "Child process killed by signal: " + std::to_string(WTERMSIG(status));
  }
#endif

  // Release GPU lock
  GpuSemaphore::release(gpuLockPath);

  // Read results from child session
  if (timedOut) {
    result.error = "Child process timed out after " + std::to_string(maxSubtaskDurationMs / 1000) + "s";
    result.exitCode = -1;
    result.success = false;
  } else if (result.exitCode == 0 && fs::exists(result.sessionPath)) {
    result.lastAssistantMessage = readLastAssistantMessage(result.sessionPath);
    result.success = !result.lastAssistantMessage.empty();
  } else {
    if (!result.error.has_value()) {
      result.error = "Child process exited with code " + std::to_string(result.exitCode);
    }
  }

  return result;
}

std::string SubAgent::readLastAssistantMessage(const std::string& sessionPath) {
  std::ifstream file(sessionPath);
  if (!file.is_open()) {
    return "";
  }

  std::string line;
  std::string lastAssistantMessage;

  while (std::getline(file, line)) {
    if (line.empty()) continue;

    try {
      nlohmann::json j = nlohmann::json::parse(line, nullptr, false);
      if (j.is_discarded()) continue;

      const std::string type = j.value("type", "");
      if (type == "message") {
        const std::string role = j.value("role", "");
        if (role == "assistant") {
          lastAssistantMessage = j.value("content", "");
        }
      }
    } catch (...) {
      // Skip malformed lines
    }
  }

  // Return first 500 characters (result summary)
  if (lastAssistantMessage.length() > 500) {
    return lastAssistantMessage.substr(0, 500) + "...";
  }

  return lastAssistantMessage;
}

}  // namespace coding_agent

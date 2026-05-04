#pragma once

#include <chrono>
#include <string>

namespace coding_agent {

/// File-based GPU lock for limiting concurrent coding-agent processes.
///
/// Implementation: writes PID + timestamp to lock file. On acquire, checks
/// if the PID is alive. If dead, treats as stale lock. If alive, waits.
/// On release, deletes the lock file.
class GpuSemaphore {
 public:
  /// Try to acquire the GPU lock. Returns true if acquired, false if busy.
  ///
  /// @param lockPath Path to the lock file.
  /// @param timeoutMs Maximum time to wait in milliseconds (default 60s).
  /// @return true if the lock was acquired, false if timeout or error.
  static bool tryAcquire(const std::string& lockPath, int timeoutMs = 60000);

  /// Release the GPU lock.
  ///
  /// @param lockPath Path to the lock file.
  static void release(const std::string& lockPath);

  /// Check if the lock is currently held by another process.
  ///
  /// @param lockPath Path to the lock file.
  /// @return true if the lock is held by a live process.
  static bool isHeld(const std::string& lockPath);

 private:
  /// Check if a process with the given PID is alive.
  ///
  /// @param pid Process ID to check.
  /// @return true if the process is alive.
  static bool isProcessAlive(pid_t pid);

  /// Read the PID from the lock file.
  ///
  /// @param lockPath Path to the lock file.
  /// @return Optional PID if file exists and is readable, nullopt otherwise.
  static std::optional<pid_t> readLockFile(const std::string& lockPath);
};

}  // namespace coding_agent

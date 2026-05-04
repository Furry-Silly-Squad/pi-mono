#include "gpu_semaphore.hpp"

#include <chrono>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <thread>

#if defined(_WIN32) || defined(_WIN64)
#include <windows.h>
#else
#include <signal.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace coding_agent {

bool GpuSemaphore::tryAcquire(const std::string& lockPath, int timeoutMs) {
  // Check if lock is already held
  if (isHeld(lockPath)) {
    // Wait with timeout
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < deadline) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
      if (!isHeld(lockPath)) {
        break;
      }
    }
    // Check again after waiting
    if (isHeld(lockPath)) {
      return false;
    }
  }

  // Acquire the lock
  std::ofstream out(lockPath, std::ios::trunc);
  if (!out.is_open()) {
    return false;
  }

#if defined(_WIN32) || defined(_WIN64)
  out << GetCurrentProcessId();
#else
  out << getpid();
#endif

  out << "\n";
  out.flush();
  return true;
}

void GpuSemaphore::release(const std::string& lockPath) {
  std::remove(lockPath.c_str());
}

bool GpuSemaphore::isHeld(const std::string& lockPath) {
  auto pid = readLockFile(lockPath);
  if (!pid.has_value()) {
    return false;
  }

  if (!isProcessAlive(*pid)) {
    // Stale lock — clean it up
    std::remove(lockPath.c_str());
    return false;
  }

  return true;
}

bool GpuSemaphore::isProcessAlive(pid_t pid) {
#if defined(_WIN32) || defined(_WIN64)
  HANDLE process = OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, static_cast<DWORD>(pid));
  bool alive = (process != nullptr);
  if (alive) {
    CloseHandle(process);
  }
  return alive;
#else
  // kill(pid, 0) returns 0 if the process exists, -1 otherwise
  return kill(pid, 0) == 0;
#endif
}

std::optional<pid_t> GpuSemaphore::readLockFile(const std::string& lockPath) {
  std::ifstream in(lockPath);
  if (!in.is_open()) {
    return std::nullopt;
  }

  std::string line;
  if (!std::getline(in, line)) {
    return std::nullopt;
  }

  try {
    return std::stol(line);
  } catch (...) {
    return std::nullopt;
  }
}

}  // namespace coding_agent

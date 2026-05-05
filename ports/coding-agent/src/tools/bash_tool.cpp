#include "tools/bash_tool.hpp"

#include <array>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <string>
#include <sys/wait.h>
#include <unistd.h>

#include <nlohmann/json.hpp>

namespace coding_agent {

std::string BashTool::name() const {
  return "bash";
}

std::string BashTool::description() const {
  return "Execute a shell command in cwd";
}

std::string BashTool::parameters_schema() const {
  return R"({"type":"object","properties":{"command":{"type":"string"}},"required":["command"]})";
}

ToolExecutionMode BashTool::execution_mode() const {
  return ToolExecutionMode::Sequential;
}

void BashTool::set_cancel_flag(std::atomic<bool>* flag) {
  cancel_flag_ = flag;
}

void BashTool::clear_cancel_flag() {
  cancel_flag_ = nullptr;
}

ToolResult BashTool::execute(const std::string& args_json, const std::string& cwd) {
  try {
    const auto args = nlohmann::json::parse(args_json);
    const std::string command = args.at("command").get<std::string>();
    const std::string wrapped = "cd \"" + cwd + "\" && " + command + " 2>&1";

    // Create pipe for child stdout
    int pipefd[2];
    if (pipe(pipefd) != 0) {
      return {
          .ok = false,
          .content = "Failed to create pipe: " + std::string(std::strerror(errno)),
          .cancelled = false};
    }

    pid_t child_pid = fork();
    if (child_pid < 0) {
      close(pipefd[0]);
      close(pipefd[1]);
      return {
          .ok = false,
          .content = "Failed to fork: " + std::string(std::strerror(errno)),
          .cancelled = false};
    }

    if (child_pid == 0) {
      // Child process
      close(pipefd[0]);  // Close read end

      // Set up new process group
      setpgid(0, 0);

      // Redirect stdout/stderr to pipe
      dup2(pipefd[1], STDOUT_FILENO);
      dup2(pipefd[1], STDERR_FILENO);
      close(pipefd[1]);

      // Execute shell command
      execl("/bin/sh", "sh", "-c", wrapped.c_str(), nullptr);

      // If we get here, exec failed
      const char* msg = "Failed to execute shell: ";
      write(STDERR_FILENO, msg, strlen(msg));
      const char* err_msg = std::strerror(errno);
      write(STDERR_FILENO, err_msg, strlen(err_msg));
      _exit(127);
    }

    // Parent process
    close(pipefd[1]);  // Close write end

    // Set the child's process group ID
    setpgid(child_pid, child_pid);

    std::array<char, 4096> buffer{};
    std::string output;
    bool cancelled = false;
    const int poll_timeout_ms = 100;  // Check cancel flag every 100ms

    while (true) {
      // Check for cancellation
      if (cancel_flag_ != nullptr && cancel_flag_->load(std::memory_order_acquire)) {
        cancelled = true;
        break;
      }

      // Poll for data with timeout
      struct pollfd pfd;
      pfd.fd = pipefd[0];
      pfd.events = POLLIN;
      int ret = poll(&pfd, 1, poll_timeout_ms);

      if (ret < 0) {
        if (errno == EINTR) continue;
        return {
            .ok = false,
            .content = "Poll error: " + std::string(std::strerror(errno)),
            .cancelled = false};
      }

      if (ret == 0) {
        // Timeout - check cancel flag and continue
        continue;
      }

      if (pfd.revents & (POLLIN | POLLPRI)) {
        ssize_t n = read(pipefd[0], buffer.data(), buffer.size());
        if (n > 0) {
          output.append(buffer.data(), n);
          if (output.size() > 100000) {
            output = output.substr(0, 100000) + "\n...[truncated]";
            break;
          }
        } else if (n == 0) {
          // EOF - child closed pipe
          break;
        } else {
          if (errno == EINTR) continue;
          return {
              .ok = false,
              .content = "Read error: " + std::string(std::strerror(errno)),
              .cancelled = false};
        }
      }

      if (pfd.revents & (POLLHUP | POLLERR)) {
        // Child closed pipe or error
        break;
      }
    }

    // If cancelled, kill the process group and wait for child
    if (cancelled) {
      killpg(child_pid, SIGKILL);
      int status;
      waitpid(child_pid, &status, 0);
      close(pipefd[0]);
      return {.ok = false, .content = "Command was cancelled", .cancelled = true};
    }

    // Wait for child to finish normally
    int status;
    waitpid(child_pid, &status, 0);

    // Close read end
    close(pipefd[0]);

    if (WIFEXITED(status)) {
      int exit_code = WEXITSTATUS(status);
      return {.ok = (exit_code == 0), .content = output, .cancelled = false};
    }

    // Child was terminated by signal (e.g., killed externally)
    if (WIFSIGNALED(status)) {
      return {.ok = false,
              .content = "Child process terminated by signal " + std::to_string(WTERMSIG(status)),
              .cancelled = false};
    }

    return {.ok = false, .content = "Child process terminated unexpectedly", .cancelled = false};

  } catch (const std::exception& ex) {
    return {.ok = false, .content = ex.what(), .cancelled = false};
  }
}

}  // namespace coding_agent

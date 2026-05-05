# PHASE-13: Bash Execution Abstraction + Shell Path / Command Prefix

## Goal

Add a pluggable `BashOperations` abstraction to the C++ bash tool and expose `shellPath` and `shellCommandPrefix` configuration, matching the TypeScript implementation. This enables:

1. **Remote bash execution** — the ability to override bash execution with a custom backend (e.g., SSH, containers) without modifying the bash tool code.
2. **Custom shell path** — users can specify an explicit shell executable (e.g., `/bin/zsh`, Cygwin's `bash.exe`) instead of relying on auto-detection.
3. **Shell command prefix** — a prefix string prepended to every bash command (e.g., `shopt -s expand_aliases`) for alias support or environment setup.

## TypeScript Reference

The TS implementation has three key pieces:

- **`BashOperations` interface** — abstracts command execution (spawn, signal handling, output streaming).
- **`createLocalBashOperations()`** — default implementation using Node.js `spawn()`.
- **`BashToolOptions`** — accepts `operations`, `commandPrefix`, `shellPath`, and `spawnHook`.
- **`getShellConfig(shellPath?)`** — cross-platform shell resolution with explicit override.
- **`getShellEnv()`** — environment augmentation (adds pi's bin directory to PATH).

## Current C++ State

The C++ `BashTool` currently:

- Hardcodes `/bin/sh -c` for command execution.
- Uses `fork()` + `pipe()` + `poll()` for async execution.
- Supports cancellation via a `std::atomic<bool>*` cancel flag.
- Processes output in a 4KB buffer with a 100KB truncation cap.
- No shell path configuration.
- No command prefix.
- No output truncation details (no temp file, no byte/line limits).
- No timeout support.

## Design

### 1. BashOperations Interface

Add a `BashOperations` abstract interface that the bash tool delegates to for process creation and output streaming. The interface mirrors the TS `BashOperations.exec()` signature.

```cpp
struct BashOutputOptions {
    /// Called for each chunk of stdout/stderr data (raw bytes, UTF-8).
    std::function<void(const std::string& data)> on_data;
    /// Optional abort signal.
    std::shared_ptr<std::atomic<bool>> abort_flag;
    /// Optional timeout in seconds (0 = no timeout).
    int timeout_seconds = 0;
    /// Optional environment override (empty = inherit parent env).
    std::map<std::string, std::string> env;
};

struct BashExecResult {
    int exit_code;  // -1 if killed/aborted
    bool aborted;
};

class BashOperations {
public:
    virtual ~BashOperations() = default;

    /// Execute a command, streaming output via on_data callback.
    /// Returns the exit code (or -1 if aborted/killed).
    virtual BashExecResult exec(
        const std::string& command,
        const std::string& cwd,
        const BashOutputOptions& options
    ) = 0;
};
```

### 2. Default Local Implementation

Create `LocalBashOperations` as the default implementation. This replaces the inline `fork()`/`pipe()`/`poll()` logic in `BashTool::execute()`.

```cpp
class LocalBashOperations final : public BashOperations {
public:
    LocalBashOperations(const std::string& shell_path = {});

    BashExecResult exec(
        const std::string& command,
        const std::string& cwd,
        const BashOutputOptions& options
    ) override;

private:
    std::string shell_path_;
    std::vector<std::string> shell_args_;
};
```

Resolution of `shell_path_` follows the TS `getShellConfig()` logic:

1. If `shell_path` is provided and exists, use it with `["-c"]`.
2. Unix: try `/bin/bash`, then `bash` on PATH, then fallback to `sh`.
3. Windows: try Git Bash in known locations, then `bash.exe` on PATH.

### 3. BashTool Changes

`BashTool` gains two new configuration fields and delegates to `BashOperations`:

```cpp
class BashTool final : public Tool {
public:
    /// Configure the tool with custom operations, prefix, and shell path.
    void configure(
        std::unique_ptr<BashOperations> operations,
        std::string shell_command_prefix,
        std::string shell_path
    );

    ToolResult execute(const std::string& args_json, const std::string& cwd) override;

private:
    std::unique_ptr<BashOperations> operations_;
    std::string shell_command_prefix_;
    std::string shell_path_;
    std::atomic<bool>* cancel_flag_ = nullptr;
};
```

In `execute()`:

1. Parse the command from `args_json`.
2. Prepend `shell_command_prefix_` if set: `prefix + "\n" + command`.
3. Resolve `cwd` (the C++ tool currently wraps as `cd "cwd" && command`).
4. Call `operations_->exec(resolved_command, cwd, options)`.
5. Process output: sanitize, truncate, build `ToolResult`.

### 4. Shell Path and Command Prefix Configuration

Add these fields to `Config` and `AgentSessionConfig`:

```cpp
struct Config {
    // ... existing fields ...
    std::string shell_path;              // "" = auto-detect
    std::string shell_command_prefix;    // "" = no prefix
};

struct AgentSessionConfig {
    // ... existing fields ...
    std::string shell_path;
    std::string shell_command_prefix;
};
```

CLI flags:

- `--shell-path <path>` — explicit shell executable path.
- `--shell-command-prefix <prefix>` — prefix prepended to every command.

`settings.json` keys:

- `shellPath` — custom shell path.
- `shellCommandPrefix` — prefix string.

### 5. Output Truncation and Temp Files

The TS implementation writes full output to a temp file when it exceeds `DEFAULT_MAX_BYTES` (~1MB), keeping a rolling tail buffer in memory. The C++ port should match this behavior:

- **In-memory buffer**: rolling tail of up to `DEFAULT_MAX_BYTES * 2` bytes.
- **Temp file**: created on first write when output exceeds `DEFAULT_MAX_BYTES`.
- **Truncation**: on completion, truncate the full output to the last N lines or bytes, write truncation metadata into `ToolResult.content`.

```cpp
struct BashTruncationResult {
    std::string content;
    bool truncated = false;
    int total_lines = 0;
    int output_lines = 0;
    int output_bytes = 0;
    bool last_line_partial = false;
};
```

### 6. Event Granularity

The TS bash tool emits `tool_execution_start`, `tool_execution_update`, and `tool_execution_end` events. The C++ port currently emits `ToolCall`, `ToolExecutionStart`, `ToolExecutionUpdate`, and `ToolExecutionEnd`. The `BashOperations` abstraction should feed these events:

- `ToolExecutionStart` — emitted before `operations_->exec()`.
- `ToolExecutionUpdate` — emitted for each data chunk from `on_data`.
- `ToolExecutionEnd` — emitted after `exec()` completes, with the final result.

### 7. Process Group Management

The current C++ implementation uses `setpgid()` and `killpg()` for process group management. This should be preserved in `LocalBashOperations`:

1. Child process sets up new process group with `setpgid(0, 0)`.
2. Parent sets child's process group with `setpgid(child_pid, child_pid)`.
3. On abort: `killpg(child_pid, SIGKILL)`.

### 8. Timeout Support

Add optional timeout to `BashOutputOptions`. `LocalBashOperations` uses `select()` or a separate timer thread to enforce the timeout:

```cpp
struct BashOutputOptions {
    // ...
    int timeout_seconds = 0;  // 0 = no timeout
};
```

When timeout expires, kill the process group and return `aborted = true`.

## File Layout

```
ports/coding-agent/src/
├── config.hpp          # Add shell_path, shell_command_prefix to Config
├── config.cpp          # Add --shell-path, --shell-command-prefix CLI flags
├── agent_session.hpp   # Add shell_path, shell_command_prefix to AgentSessionConfig
├── agent_session.cpp   # Pass shell config to BashTool
├── tools/
│   ├── bash_operations.hpp   # NEW: BashOperations interface + LocalBashOperations
│   ├── bash_operations.cpp   # NEW: LocalBashOperations implementation
│   ├── bash_tool.hpp         # ADD: configure() method, shell_command_prefix_, shell_path_ fields
│   └── bash_tool.cpp         # MODIFY: delegate to operations_, prepend prefix, temp file truncation
├── utils/
│   ├── shell.hpp           # NEW: shell resolution utilities (cross-platform)
│   └── shell.cpp           # NEW: get_shell_config(), get_shell_env()
└── truncate.hpp            # NEW: output truncation utilities (matching TS truncate.ts)
```

## Shell Resolution (cross-platform)

Implement `get_shell_config()` in `utils/shell.{hpp,cpp}`:

```cpp
struct ShellConfig {
    std::string shell;
    std::vector<std::string> args;
};

/// Resolve shell configuration.
/// If custom_shell_path is non-empty and exists, use it.
/// Otherwise, auto-detect based on platform.
ShellConfig get_shell_config(const std::string& custom_shell_path = {});
```

Platform resolution:

- **Unix**: `/bin/bash` → `bash` on PATH → `sh`.
- **Windows**: Git Bash (`ProgramFiles\Git\bin\bash.exe`, `ProgramFiles(x86)\Git\bin\bash.exe`) → `bash.exe` on PATH.

## Output Truncation

Implement `truncate_tail()` matching the TS `truncateTail()`:

```cpp
struct TruncationResult {
    std::string content;
    bool truncated = false;
    int total_lines = 0;
    int output_lines = 0;
    size_t max_bytes = 0;
    bool last_line_partial = false;
};

/// Truncate output to the last N lines or bytes (whichever is hit first).
/// Returns the truncated content and metadata.
TruncationResult truncate_tail(const std::string& input,
                                int max_lines = 50,
                                size_t max_bytes = 512 * 1024);
```

## Sanitization

Port `sanitize_binary_output()` from TS `utils/shell.ts` to C++:

```cpp
/// Remove control characters, lone surrogates, and Unicode format characters
/// that could cause display issues.
std::string sanitize_binary_output(const std::string& input);
```

## C++ vs TS Differences

| Aspect | TS | C++ (target) |
|--------|----|---------------|
| Process spawning | `child_process.spawn()` | `fork()` + `exec()` |
| Output streaming | Node.js stream events | `poll()` loop with callback |
| Cancellation | `AbortSignal` | `std::atomic<bool>*` |
| Timeout | `setTimeout` in spawn | `select()` or timer thread |
| Process tree kill | `killProcessTree(pid)` | `killpg()` (process group) |
| Child tracking | `trackedDetachedChildPids` set | Same pattern (track PIDs for cleanup) |
| Shell env augmentation | Adds pi bin dir to PATH | Same (add configured bin dir to PATH) |
| Temp file path | `tmpdir()` + random hex | `/tmp/pi-bash-<hex>.log` |

## Implementation Order

1. **Shell resolution utilities** (`utils/shell.{hpp,cpp}`) — cross-platform shell detection.
2. **Output truncation** (`truncate.{hpp,cpp}`) — tail truncation logic.
3. **`BashOperations` interface + `LocalBashOperations`** — the abstraction layer.
4. **`BashTool::configure()`** — wire up operations, prefix, shell path.
5. **Config fields** — add `shell_path` and `shell_command_prefix` to `Config` and `AgentSessionConfig`.
6. **CLI flags** — `--shell-path` and `--shell-command-prefix`.
7. **Temp file + truncation in execute()** — full output handling.
8. **Timeout support** — optional in `BashOutputOptions`.

## Testing

- Unit tests for `get_shell_config()` on each platform (Unix, Windows mock).
- Unit tests for `truncate_tail()` — edge cases: empty input, single line, multi-line, byte limit hit before line limit, partial last line.
- Unit tests for `sanitize_binary_output()` — control chars, surrogates, format chars.
- Integration test: spawn `LocalBashOperations`, verify output streaming, cancellation, timeout, exit codes.
- Integration test: verify command prefix is prepended correctly.
- Integration test: verify custom shell path is used when provided.

## Dependencies

- No new dependencies. Uses existing nlohmann/json, stdlib.
- Cross-platform shell detection uses `<sys/stat.h>` (Unix) and `<windows.h>` (Windows).

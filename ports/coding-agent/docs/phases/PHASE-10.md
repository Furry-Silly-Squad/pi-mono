# Phase 10: Self-Rebuild and Restart

## Goal

Add a `/rebuild` command to interactive mode that:

1. Rebuilds the `coding-agent` binary from source using CMake
2. If the build succeeds, replaces the current process with the new binary
3. Preserves the same original command-line arguments
4. Continues the same session seamlessly (session is persisted to `.jsonl`)

This enables the agent to efficiently develop and iterate on its own code.

## Implementation Status: **Complete**

Implemented in commit `c19840ca`.

## Design Decisions

### 1. Build directory detection

The build directory contains `CMakeCache.txt` and the compiled binary. Detection strategy (current implementation):

- **Primary**: Read `/proc/self/exe` to get the absolute path of the running binary, then take its parent directory. Check for `CMakeCache.txt` there.
- **Secondary**: Check for `CMakeCache.txt` in `cwd/build/`.
- **Fallback**: Return `std::nullopt`; the `/rebuild` command prints an error message.

The build directory path is detected once at startup and stored in a `static const` variable inside `run_interactive_mode()`.

### 2. Source directory detection

Not implemented as a separate step. The build directory detection relies on `/proc/self/exe` (Linux) or `argv[0]`, then checks for `CMakeCache.txt` in the parent directory. No explicit `CMakeLists.txt` detection.

### 3. Self-replacement via `execvp`

Use `execvp(argv[0], argv)` to replace the current process. This is the cleanest approach because:

- No child process lifecycle management
- stdin/stdout/stderr are preserved (readline continues working)
- Signal handlers are reset (but `SIGINT` handler in `interactive_mode.cpp` is set up at the start of the new process, so this is fine)
- The session file path is unchanged — the new process opens the same session
- All file descriptors are preserved

### 4. Arguments preservation

`argc` and `argv` are threaded through the call stack:

```
main(argc, argv)
  -> run_agent(argc, argv)
    -> run_interactive_mode(agent, interactive_debug, argc, argv)
```

`argc` is cast to `void` inside the function body (unused except for `execvp`). `argv` is passed to `execvp(argv[0], argv)` on success.

### 5. Confirmation

The `/rebuild` command uses a **single-step confirmation** via a `rebuild_pending` flag:

- First invocation of `/rebuild`: sets `rebuild_pending = true`, prints a warning, and tells the user to type `/rebuild confirm`
- `/rebuild confirm` / `/rebuild yes` / `/rebuild y`: if `rebuild_pending` is true, proceeds with the build; if false, falls through to the warning path
- After execution, `rebuild_pending` is reset to `false`

### 6. Build output handling

The build command uses shell redirection:

```bash
cmake --build "<build_dir>" --target coding-agent 2>&1
```

Output (stdout + stderr) goes to the terminal via `std::system()`. No filtering or redirection to stderr.

### 7. What happens on failure

If the build fails:
- The old binary remains running (`execvp` was never called)
- The agent prints an error message with the exit code
- The session is intact — the agent can continue working or retry the rebuild

If `execvp` fails (extremely unlikely):
- An error message is printed with `strerror(errno)`
- The old binary remains running

## Implementation Details

### Files modified

1. **`src/modes/interactive_mode.hpp`** — `run_interactive_mode()` takes `int argc, char** argv` parameters
2. **`src/modes/interactive_mode.cpp`** — Implements `/rebuild` command, `detect_build_dir()`, `run_build_command()`, and `confirm_destructive_bash_on_tty()`
3. **`src/agent.cpp`** — Threads `argc`/`argv` from `run_agent()` to `run_interactive_mode()`
4. **`src/agent_session.hpp`/`cpp`** — Added `set_destructive_bash_confirm()` and `destructive_bash_confirm_` member for the bash gate mechanism

### Key functions

```cpp
std::optional<std::string> detect_build_dir(const char* argv0);
int run_build_command(const std::string& build_dir);
bool execute_rebuild(const std::string& build_dir, char** argv,
                     const struct sigaction& original_sigint_handler);
bool confirm_destructive_bash_on_tty(const std::string& command);
```

`execute_rebuild()` encapsulates the full build-and-restart sequence: resets SIGINT to `SIG_DFL`, runs the build, restores the handler, checks the result, and calls `execvp` on success.

### Signal handling during build

During the build, the `SIGINT` handler is temporarily reset to `SIG_DFL` so that Ctrl-C kills the build subprocess. After the build completes (success or failure), the original handler is restored.

### Token budget formatting

A `format_token_budget()` function formats the token usage with color-coded percentages (green <50%, yellow 50-80%, red >80%) and displays compaction proximity. This is shown after every turn.

### Additional features in interactive mode

- `/stats` / `/session` — detailed session stats
- `/tokens` — token usage summary
- `/compact` — manual compaction trigger
- `/thinking` — cycle thinking level
- `/queues` — show steering and follow-up queue state
- `/clear-queues` — clear all pending queues
- `/new` — create a new session
- `/branch` / `/branch summary` / `/branch from:<id>` — session branching
- `destructive_bash_confirm` — bash gate that prompts on `/dev/tty` for destructive commands

## Risks and Mitigations

| Risk | Mitigation |
|------|-----------|
| Build fails, agent is in a bad state | Old binary continues running; session is safe |
| Agent is mid-turn when rebuild is triggered | Confirmation step gives time to finish |
| Build directory not found | Clear error message with instructions |
| Source directory not found | Clear error message with instructions |
| `execvp` fails (unlikely) | Falls back to printing an error; old binary still running |
| Readline state lost after execvp | `execvp` preserves file descriptors; readline reinitializes on the new process |
| `/proc/self/exe` unavailable (non-Linux) | Falls back to `argv[0]` and `cwd/build/` check |
| Build interrupted by Ctrl-C | `SIGINT` handler reset to `SIG_DFL` during build; child process is terminated |

## Code Notes

- The `/rebuild` implementation has some duplicated code between the `confirm` path and the `rebuild_pending` path. Both execute the build and `execvp` with the same signal handling. This could be refactored into a helper function.
- The `argc` parameter is currently unused in the function body (cast to `void`) — it exists solely for `execvp(argv[0], argv)`.
- `detect_build_dir` is Linux-specific due to `/proc/self/exe`. On macOS, the fallback to `argv[0]` + `cwd/build/` is the primary path.

## Future considerations

- **`/rebuild --force`**: Skip confirmation, rebuild immediately
- **`/rebuild --dry-run`**: Show what would be built without actually building
- **Auto-rebuild on code changes**: Detect if source files changed since last build and offer auto-rebuild
- **Cross-platform build dir detection**: Replace `/proc/self/exe` with a more portable approach (e.g., `dl_iterate_phdr` on Linux, `_NSGetExecutablePath` on macOS)
- **Refactor duplicated build execution code** into a shared helper

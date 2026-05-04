# Phase 10: Self-Rebuild and Restart

## Goal

Add a `/rebuild` command to interactive mode that:

1. Rebuilds the `coding-agent` binary from source using CMake
2. If the build succeeds, replaces the current process with the new binary
3. Preserves the same original command-line arguments
4. Continues the same session seamlessly (session is persisted to `.jsonl`)

This enables the agent to efficiently develop and iterate on its own code.

## Feasibility

**Yes, feasible.** The approach is straightforward:

- **Build**: `cmake --build <build-dir>` runs the existing build system
- **Self-replacement**: `execvp(argv[0], argv)` replaces the current process with the new binary — no child process, no cleanup, no signal handling changes
- **Session continuity**: Session state lives in the `.jsonl` file on disk, so the new process picks up exactly where the old one left off

## Implementation Status

**Phase 10 is complete.** All core functionality is implemented and committed.

### Implemented

- [x] `argc`/`argv` threaded through `run_agent()` → `run_interactive_mode()`
- [x] Build directory detection at startup (primary: `/proc/self/exe` parent dir, fallback: `cwd/build/`)
- [x] `CMakeCache.txt` existence check to validate detected build directory
- [x] `/rebuild` command with two-step confirmation flow
- [x] `/rebuild --force` command with no confirmation
- [x] `cmake --build <build-dir> --target coding-agent` execution via `system()`
- [x] `execvp(argv[0], argv)` process replacement on success
- [x] SIGINT handler reset to `SIG_DFL` during build (allows Ctrl-C to kill build)
- [x] Build failure handling (old binary continues running)
- [x] `execvp` failure handling (falls back to error message)
- [x] Startup log message when build directory is detected
- [x] Build/rebuild logic extracted into `do_rebuild` lambda to avoid code duplication

### Not Implemented (Future Work)

- [ ] Source directory detection (mentioned in original design, not needed since build dir detection suffices)
- [ ] `/rebuild --dry-run` — show what would be built without building

## Design Decisions

### 1. Build directory detection

The build directory contains `CMakeCache.txt` and the compiled binary. Detection strategy:

- **Primary**: Read `/proc/self/exe` via `readlink()`, take parent directory, check for `CMakeCache.txt`
- **Secondary**: Check `cwd/build/` for `CMakeCache.txt`
- **Fallback**: `build_dir` is `std::nullopt`; `/rebuild` prints an error and continues

The build directory path is detected once at startup via a `static const` variable.

### 2. Self-replacement via `execvp`

Use `execvp(argv[0], argv)` to replace the current process. This is the cleanest approach because:

- No child process lifecycle management
- stdin/stdout/stderr are preserved (readline continues working)
- Signal handlers are reset (but `SIGINT` handler in `interactive_mode.cpp` is set up at the start of the new process, so this is fine)
- The session file path is unchanged — the new process opens the same session
- All file descriptors are preserved

### 3. Arguments preservation

`argv` is passed through the call stack; `argc` is accepted but not directly used (only `argv[0]` is needed for `execvp`):

```
main(argc, argv)
  -> run_agent(argc, argv)
    -> run_interactive_mode(agent, interactive_debug, argc, argv)
```

The `AgentSession` class does NOT need to know about `argc`/`argv` — only `run_interactive_mode` needs them.

### 4. Confirmation

The `/rebuild` command supports three modes:

- **No arguments** (`/rebuild`): Two-step confirmation — first invocation prints a warning, second invocation proceeds
- **`/rebuild confirm`** (or `yes`/`y`): Proceeds with build if `rebuild_pending` is set (second step of two-step flow)
- **`/rebuild --force`**: Skips all confirmation and rebuilds immediately

Rationale: Two-step confirmation protects against accidental mid-turn rebuilds. `--force` is for rapid development iteration where the agent knows it is in a safe state (e.g., after finishing a turn).

### 5. Build output handling

During the build, CMake output is captured via `2>&1` in the shell command string passed to `system()`. After the build, a success/failure message is printed.

### 6. What happens on failure

If the build fails:
- The old binary remains running (`execvp` was never called)
- The agent prints an error message with the exit code
- The session is intact — the agent can continue working or retry the rebuild

If `execvp` fails (extremely unlikely):
- Falls back to printing `strerror(errno)` and continues with the old binary

## Implementation Plan

### Files modified

1. **`src/modes/interactive_mode.hpp`** — Added `argc`/`argv` parameters to `run_interactive_mode()`
2. **`src/modes/interactive_mode.cpp`** — Implemented `/rebuild` command logic, build dir detection, `run_build_command()`
3. **`src/agent.cpp`** — Threaded `argc`/`argv` to `run_interactive_mode()`

### Implementation steps completed

#### Step 1: Thread argc/argv ✅

```cpp
// interactive_mode.hpp
int run_interactive_mode(AgentSession& agent, bool interactive_debug,
                         int argc, char** argv);
```

#### Step 2: Detect build directory at startup ✅

In `run_interactive_mode()`, detect the build directory once:

```cpp
static const auto build_dir = detect_build_dir(argv[0]);
```

`detect_build_dir()` uses `/proc/self/exe` → parent dir → `CMakeCache.txt` check, with `cwd/build/` fallback.

#### Step 3: Implement `/rebuild` command ✅

```
/rebuild          -> Print warning, set rebuild_pending = true
/rebuild confirm  -> Run cmake --build <build-dir>
/rebuild yes      -> Same as confirm
/rebuild y        -> Same as confirm
```

The build command:
```bash
cmake --build <build-dir> --target coding-agent 2>&1
```

Check the exit code. If 0, call `execvp(argv[0], argv)`. If non-zero, print error.

#### Step 4: Handle SIGINT during build ✅

During the build, `SIGINT` is set to `SIG_DFL` so the default behavior (terminate the build process) takes effect. After the build completes, the original handler is restored via `sigaction`.

## Risks and Mitigations

| Risk | Mitigation | Status |
|------|-----------|--------|
| Build fails, agent is in a bad state | Old binary continues running; session is safe | ✅ Implemented |
| Agent is mid-turn when rebuild is triggered | Confirmation step gives time to finish | ✅ Implemented |
| Build directory not found | Clear error message with instructions | ✅ Implemented |
| `execvp` fails (unlikely) | Falls back to printing an error; old binary still running | ✅ Implemented |
| Readline state lost after execvp | `execvp` preserves file descriptors; readline reinitializes on the new process | ✅ Verified |

## Future considerations

- **`/rebuild --force`**: Skip confirmation, rebuild immediately
- **`/rebuild --dry-run`**: Show what would be built without actually building
- **Auto-rebuild on code changes**: Detect if source files changed since last build and offer auto-rebuild (probably not needed for Phase 10)

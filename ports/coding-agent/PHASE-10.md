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

## Design Decisions

### 1. Build directory detection

The build directory contains `CMakeCache.txt` and the compiled binary. Detection strategy:

- **Primary**: Check for `CMakeCache.txt` in the sibling `build/` directory relative to the source (i.e., `../build/` from `src/`)
- **Secondary**: Check for `CMakeCache.txt` in the current working directory
- **Fallback**: Error with a message telling the user the build directory

The build directory path is detected once at startup and stored.

### 2. Source directory detection

The source directory contains `CMakeLists.txt`. Detection:

- Look for `CMakeLists.txt` in the parent directory of the binary, or in `../` relative to the build directory
- Could also use `argv[0]` to determine the binary path and work from there

### 3. Self-replacement via `execvp`

Use `execvp(argv[0], argv)` to replace the current process. This is the cleanest approach because:

- No child process lifecycle management
- stdin/stdout/stderr are preserved (readline continues working)
- Signal handlers are reset (but `SIGINT` handler in `interactive_mode.cpp` is set up at the start of the new process, so this is fine)
- The session file path is unchanged — the new process opens the same session
- All file descriptors are preserved

### 4. Arguments preservation

`argc` and `argv` must be threaded through the call stack:

```
main(argc, argv)
  -> run_agent(argc, argv)
    -> run_interactive_mode(agent, interactive_debug, argc, argv)
```

The `AgentSession` class does NOT need to know about `argc`/`argv` — only `run_interactive_mode` needs them.

### 5. Confirmation

The `/rebuild` command should **require confirmation** before building:

- First invocation of `/rebuild`: prints a warning about rebuilding and waits for `/rebuild confirm` or `/rebuild yes`
- Second invocation with confirmation: proceeds with the build

Rationale: The agent might have pending tool calls or an in-progress turn. Rebuilding mid-turn would lose that state (though the session file is safe). Confirmation gives the agent a chance to finish its current work.

### 6. Build output handling

During the build, CMake output should be printed to `stderr` so it doesn't interfere with the readline prompt or the agent's output stream. After the build, a success/failure message is printed.

### 7. What happens on failure

If the build fails:
- The old binary remains running (execvp was never called)
- The agent prints an error message
- The session is intact — the agent can continue working or retry the rebuild

## Implementation Plan

### Files to modify

1. **`src/modes/interactive_mode.hpp`** — Add `argc`/`argv` parameters to `run_interactive_mode()`
2. **`src/modes/interactive_mode.cpp`** — Implement `/rebuild` command logic
3. **`src/agent.cpp`** — Thread `argc`/`argv` to `run_interactive_mode()`

### Implementation steps

#### Step 1: Thread argc/argv

```cpp
// interactive_mode.hpp
int run_interactive_mode(AgentSession& agent, bool interactive_debug,
                         int argc, char** argv);
```

#### Step 2: Detect build directory at startup

In `run_interactive_mode()`, detect the build directory once:

```cpp
static std::optional<std::string> detect_build_dir() {
    // Check ../build/ relative to source
    // Check ./build/ relative to cwd
    // Check for CMakeCache.txt
}
```

#### Step 3: Implement `/rebuild` command

```
/rebuild          -> Print warning, set rebuild_pending = true
/rebuild confirm  -> Run cmake --build <build-dir>
/rebuild yes      -> Same as confirm
```

The build command:
```bash
cmake --build <build-dir> --target coding-agent 2>&1
```

Check the exit code. If 0, call `execvp(argv[0], argv)`. If non-zero, print error.

#### Step 4: Handle SIGINT during build

During the build, `SIGINT` should be handled gracefully:
- The readline signal handler sets `global_cancel_flag`
- But the build runs in a subprocess (via `system()` or `popen()`)
- We should catch `SIGINT` during the build and abort it, then print a message

Actually, `execvp` replaces the process, so we don't need to worry about signal handling during the build itself — the build runs in a child process via `system()`/`popen()`, and `SIGINT` to the parent will kill the child.

## Risks and Mitigations

| Risk | Mitigation |
|------|-----------|
| Build fails, agent is in a bad state | Old binary continues running; session is safe |
| Agent is mid-turn when rebuild is triggered | Confirmation step gives time to finish |
| Build directory not found | Clear error message with instructions |
| Source directory not found | Clear error message with instructions |
| `execvp` fails (unlikely) | Falls back to printing an error; old binary still running |
| Readline state lost after execvp | `execvp` preserves file descriptors; readline reinitializes on the new process |

## Future considerations

- **`/rebuild --force`**: Skip confirmation, rebuild immediately
- **`/rebuild --dry-run`**: Show what would be built without actually building
- **Auto-rebuild on code changes**: Detect if source files changed since last build and offer auto-rebuild (probably not needed for Phase 10)

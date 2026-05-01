# Phase 6: Agent Interruption & TUI Feedback

## Goal
Allow the user to interrupt long-running agent operations (LLM responses, tool execution) and provide visual feedback during waiting periods so the user always knows what the program is doing.

## Complexity: Medium-High

## Prerequisites
- Phase 5 complete: token feedback working.
- Build passes with zero errors and zero new warnings.

---

## Task Status Summary

| # | Task | Status |
|---|------|--------|
| 1 | Interruptible HTTP request in provider | Done |
| 2 | Ctrl+C / key-based interrupt in agent loop | Done |
| 3 | TUI loading animation during LLM wait | Done |
| 4 | Tool execution status display | Done |
| 5 | Cancellation feedback with graceful cleanup | Done |

---

## Tasks

### 1. Interruptible HTTP request in provider

**Must-have**

- [x] Add a cancellation mechanism to `LlamaCppProvider::chat()`:
  - Use `curl_easy_setopt(curl, CURLOPT_PROGRESSFUNCTION, ...)` with a progress callback that checks `cancel_flag`.
  - Support soft cancellation via a `std::atomic<bool>* cancel_flag` parameter on `chat()`.
  - If `cancel_flag` is set to true mid-stream, abort the curl request and return `false` with error `"interrupted"`.
- [x] Add `cancel()` method to `Provider` interface (virtual, no-op default).
- [x] In non-streaming mode: abort immediately on cancel (progress callback checks flag).
- [x] In streaming mode: abort mid-stream, discard partial content, return `false`.

**Current code state:**
- `LlamaCppProvider::chat()` uses `curl_easy_perform()` with a 300s timeout.
- Added `cancel_flag` parameter to `chat()` and progress callback that checks the flag.
- Added `cancel()` method to `Provider` interface.
- Progress callback returns non-zero on cancel, which aborts the curl transfer.

**Files:** `providers/provider.hpp`, `providers/llama_cpp_provider.cpp`

---

### 2. Ctrl+C / key-based interrupt in agent loop

**Must-have**

- [x] Add a `std::atomic<bool> global_cancel_flag` in `interactive_mode.cpp`.
- [x] In `interactive_mode.cpp`: install a signal handler for SIGINT (Ctrl+C) via `sigaction()` that sets `global_cancel_flag = true`.
- [x] In `agent_loop.cpp`: accept `std::atomic<bool>* cancel_flag` parameter and pass it to `provider.chat()`.
- [x] In `agent_loop.cpp`: check for `"interrupted"` error and return early with `RunResult{.ok = false, .error = "interrupted"}`.
- [x] In `interactive_mode.cpp`: print `[interrupted]` when interrupted and clear the flag after handling.
- [x] In `LlamaCppProvider::chat()`: progress callback checks `cancel_flag` and aborts curl transfer.

**Current code state:**
- `interactive_mode.cpp` has `global_cancel_flag`, `signal_handler()`, and `sigaction(SIGINT, ...)` installed.
- `agent_loop.cpp` accepts `cancel_flag` parameter and passes it to `provider.chat()`.
- `LlamaCppProvider` has progress callback that checks `cancel_flag` and aborts on true.
- `interactive_mode.cpp` checks for `"interrupted"` error and prints `[interrupted]`.
- Flag is reset at the start of each new request.

**Nice-to-have**

- [ ] Add a `/cancel` command (in addition to Ctrl+C) that sets the interrupt flag.
- [ ] After interrupt, allow the user to continue the session (already works — returns to prompt).

**Files:** `modes/interactive_mode.cpp`, `agent_loop.cpp`, `agent_loop.hpp`, `providers/provider.hpp`, `providers/llama_cpp_provider.cpp`

---

### 3. TUI loading animation during LLM wait

**Must-have**

- [x] Create a `TuiAnimation` class (`tui_animation.hpp`/`tui_animation.cpp`) that:
  - Runs in a background thread.
  - Displays a loading indicator using Unicode spinner frames (`⠋ ⠙ ⠹ ⠸ ⠼ ⠴ ⠦ ⠧ ⠇ ⠏`).
  - Updates the display at ~10Hz (100ms interval) without interfering with stdout.
  - Stops when told to (via a `std::atomic<bool>` flag).
- [x] In `interactive_mode.cpp`:
  - Before calling `provider.chat()`, start the animation with `AnimationState::Thinking`.
  - After the call returns (success, failure, or interrupt), stop the animation.
  - Switch to `AnimationState::Generating` on first chunk received.
  - Print the response normally after the animation stops.
- [x] The animation is a single line that updates in place using `\033[2K\r` (clear line + carriage return).
- [x] Cursor is hidden during animation and shown when stopped.

**Current code state:**
- `TuiAnimation` class implemented with background thread, spinner frames, and state management.
- `interactive_mode.cpp` starts animation before `run_agent_loop()`, stops after.
- Animation switches to "generating" on first chunk via updated `ChunkCallback`.
- ANSI escape codes: `\033[?25l` (hide cursor), `\033[?25h` (show cursor), `\033[2K\r` (clear line).

**Nice-to-have**

- [ ] Different animations for different states (already implemented: Thinking/Generating/Running).
- [ ] Configurable animation style (dots, bars, text).

**Files:** `modes/tui_animation.hpp`, `modes/tui_animation.cpp`, `modes/interactive_mode.cpp`

---

### 4. Tool execution status display

**Must-have**

- [ ] Before each tool dispatch, print a status line:
  - `[tool: <name>] running...`
- [ ] After tool completes, print:
  - `[tool: <name>] done (<duration>ms)` or `[tool: <name>] failed: <error>`
- [ ] Use `\r` to update in place (same line), so it doesn't break the output flow.
- [ ] If the tool runs for >5 seconds, switch to a spinning indicator:
  - `[tool: <name>] ⠋ running...` (updates every 100ms)

**Current code state:**
- Tool results are printed inline as `ok/error: <content>` in the chat history.
- No visual feedback during tool execution.
- Long-running tools (e.g., `bash` with a slow command) give no indication they're still running.

**Nice-to-have**

- [ ] Show estimated remaining time for tools that report progress.
- [ ] Allow user to interrupt a running tool (via Ctrl+C, which sets the interrupt flag).

**Files:** `agent_loop.cpp`

---

### 5. Cancellation feedback with graceful cleanup

**Must-have**

- [ ] When the user interrupts:
  - Stop any in-progress LLM request (via `curl_easy_pause` or closing the connection).
  - Stop any in-progress tool execution (send SIGINT to child processes if applicable).
  - Stop the TUI animation.
  - Print a clear status message: `[interrupted] operation cancelled`.
  - Return to the prompt (don't exit).
- [ ] Preserve the partial state:
  - If the LLM was mid-response, discard the partial response (don't add it to history).
  - If a tool was mid-execution, discard its partial output.
  - History up to the last complete turn is preserved.
- [ ] After interrupt, the user can issue a new prompt and continue the session normally.

**Current code state:**
- No graceful interruption.
- No cleanup on abort.
- History is not rolled back on partial responses.

**Nice-to-have**

- [ ] After interrupt, offer the user options:
  - Continue with a new prompt (default)
  - Retry the interrupted turn
  - Exit the session
- [ ] Log interrupted operations for debugging.

**Files:** `modes/interactive_mode.cpp`, `agent_loop.cpp`, `providers/llama_cpp_provider.cpp`

---

## Validation

```bash
# Build
cmake -S ports/coding-agent -B ports/coding-agent/build
cmake --build ports/coding-agent/build -j

# Manual scenario 1: interrupt during LLM response
ports/coding-agent/build/coding-agent --base-url http://... --prompt "write a very long essay"
# → While waiting for response, see animation: "⠋ thinking..."
# → While streaming, see: "⠋ generating..."
# → Press Ctrl+C during streaming
# → See: "[interrupted] operation cancelled"
# → Return to prompt, session continues

# Manual scenario 2: interrupt during tool execution
ports/coding-agent/build/coding-agent --base-url http://... --prompt "run a slow bash command"
# → See: "[tool: bash] ⠋ running..."
# → Press Ctrl+C while tool is running
# → See: "[interrupted] operation cancelled"
# → Return to prompt, session continues

# Manual scenario 3: normal operation (no interrupt)
# → Animation starts, response arrives, animation stops
# → Response displayed normally
# → No visual artifacts
```

## Acceptance Criteria

- [ ] `Provider::cancel()` method exists and can abort an in-flight HTTP request.
- [ ] Ctrl+C sets an interrupt flag and returns to prompt (doesn't kill the process).
- [ ] TUI animation displays while waiting for LLM response.
- [ ] Animation stops when response arrives or is interrupted.
- [ ] Tool execution shows status (`[tool: <name>] running...`).
- [ ] After interrupt, partial responses are discarded and history is preserved up to last complete turn.
- [ ] User can continue the session after an interrupt.
- [ ] No visual artifacts when no interrupt occurs.
- [ ] Build passes with zero errors and zero new warnings.

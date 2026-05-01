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
| 1 | Interruptible HTTP request in provider | Not started |
| 2 | Ctrl+C / key-based interrupt in agent loop | Not started |
| 3 | TUI loading animation during LLM wait | Not started |
| 4 | Tool execution status display | Not started |
| 5 | Cancellation feedback with graceful cleanup | Not started |

---

## Tasks

### 1. Interruptible HTTP request in provider

**Must-have**

- [ ] Add a cancellation mechanism to `LlamaCppProvider::chat()`:
  - Use `curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, ...)` for hard timeout.
  - Support soft cancellation via a `std::atomic<bool>* cancel_flag` parameter on `chat()`.
  - If `cancel_flag` is set to true mid-stream, abort the curl request and return `false` with error `"interrupted"`.
- [ ] Add `cancel()` method to `Provider` interface (virtual, no-op default).
- [ ] In non-streaming mode: abort immediately on cancel.
- [ ] In streaming mode: flush any partial content received, set `response.content` to partial, return `false`.

**Current code state:**
- `LlamaCppProvider::chat()` uses `curl_easy_perform()` with a 300s timeout.
- No way to abort mid-request.
- `Provider` interface has no cancellation method.

**Nice-to-have**

- [ ] Support configurable timeout per-request (via `ChatRequest` field).

**Files:** `providers/provider.hpp`, `providers/llama_cpp_provider.cpp`

---

### 2. Ctrl+C / key-based interrupt in agent loop

**Must-have**

- [ ] Add a `std::atomic<bool> interrupted_` flag to `SessionStore` or a shared context object.
- [ ] In `interactive_mode.cpp`: install a signal handler for SIGINT (Ctrl+C) that sets `interrupted_ = true`.
- [ ] In `agent_loop.cpp`: check `interrupted_` before each LLM call and tool dispatch. If set:
  - Return early with `RunResult{.ok = false, .error = "interrupted"}`.
  - Print `[interrupted]` to the user.
  - Clear the flag after handling.
- [ ] In `LlamaCppProvider::chat()`: pass a pointer to `interrupted_` so the HTTP request can be aborted mid-stream.

**Current code state:**
- No signal handling in `interactive_mode.cpp`.
- No interruption mechanism in `agent_loop.cpp` or `Provider`.
- Ctrl+C would kill the process entirely.

**Nice-to-have**

- [ ] Add a `/cancel` command (in addition to Ctrl+C) that sets the interrupt flag.
- [ ] After interrupt, allow the user to continue the session (don't exit).

**Files:** `modes/interactive_mode.cpp`, `agent_loop.cpp`, `session.hpp`, `providers/provider.hpp`

---

### 3. TUI loading animation during LLM wait

**Must-have**

- [ ] Create a `TuiAnimation` class (or similar) that:
  - Runs in a background thread.
  - Displays a loading indicator (e.g., `⠋ ⠙ ⠹ ⠸ ⠼ ⠴ ⠦ ⠧ ⠇ ⠏` rotating dots, or a simple `...` spinner).
  - Updates the display at ~10Hz without interfering with stdout.
  - Stops when told to (via a `std::atomic<bool>` flag).
- [ ] In `interactive_mode.cpp`:
  - Before calling `provider.chat()`, start the animation.
  - After the call returns (success, failure, or interrupt), stop the animation.
  - Print the response normally after the animation stops.
- [ ] The animation should be a single line (e.g., `⠋ thinking...`) that updates in place using `\r` or ANSI cursor control.

**Current code state:**
- `interactive_mode.cpp` uses a simple `ChunkCallback` that prints chunks as they arrive.
- No visual feedback while waiting for the LLM to start generating.
- User sees nothing between pressing Enter and the first chunk arriving (can be several seconds).

**Nice-to-have**

- [ ] Different animations for different states:
  - Waiting for LLM: rotating dots (`⠋ thinking...`)
  - Streaming content: subtle pulse (`⠋ generating...`)
  - Running tool: tool name (`⠋ running: bash...`)
  - Interrupted: `[interrupted]` with a distinct marker
- [ ] Configurable animation style (dots, bars, text).

**Files:** `modes/interactive_mode.cpp`, `modes/tui_animation.hpp`, `modes/tui_animation.cpp`

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

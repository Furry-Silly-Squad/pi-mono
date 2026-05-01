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
| 2 | Ctrl+C / cooperative cancel in agent loop | Done (interactive TUI only; see notes) |
| 3 | TUI loading animation during LLM wait | Done |
| 4 | Tool execution status display | Partial (basic lines; no duration, no slow-tool spinner, no in-place `\r`) |
| 5 | Cancellation feedback with graceful cleanup | Partial (LLM + UI; tools not cooperatively cancelled; session vs history caveats) |

---

## Tasks

### 1. Interruptible HTTP request in provider

**Must-have**

- [x] Cancellation via `std::atomic<bool>* cancel_flag` passed to `LlamaCppProvider::chat()`:
  - `CURLOPT_NOPROGRESS` / `CURLOPT_PROGRESSFUNCTION` invokes `progress_callback`, which reads `cancel_flag` and returns non-zero to abort the transfer (`CURLE_ABORTED_BY_CALLBACK` → error `"interrupted"`).
  - Streaming path: `write_stream_callback` checks `cancel_flag` before processing chunks; sets internal error and returns 0 to abort.
- [x] `Provider` is abstract with `virtual void cancel() = 0`; `LlamaCppProvider` implements `cancel()` (sets internal `interrupted_`; **not** wired into the active transfer today — cooperative abort uses `cancel_flag`, not `cancel()` alone).
- [x] Non-streaming and streaming both abort when `cancel_flag` becomes true mid-request.

**Files:** `providers/provider.hpp`, `providers/llama_cpp_provider.cpp`

---

### 2. Ctrl+C / key-based interrupt in agent loop

**Must-have**

- [x] `std::atomic<bool> global_cancel_flag` in `interactive_mode.cpp`.
- [x] `sigaction(SIGINT, ...)` sets `global_cancel_flag = true` (does not exit the process).
- [x] `run_agent_loop(..., &global_cancel_flag, ...)` passes the pointer into `provider.chat(..., cancel_flag)`.
- [x] On `error == "interrupted"`, returns `RunResult{.ok = false, .error = "interrupted"}`.
- [x] Interactive mode prints `\n[interrupted]\n` on that result and resets `global_cancel_flag` at the **start** of each new prompt turn.

**Scope**

- [x] **`run_print_mode`** calls `run_agent_loop` **without** `cancel_flag` (defaults to `nullptr`), so `--prompt` / piped stdin runs have **no** cooperative cancellation path — SIGINT keeps default process behavior unless changed elsewhere.

**Nice-to-have**

- [ ] `/cancel` command (only Ctrl+C today).
- [x] After interrupt, user stays in the REPL and can enter a new prompt.

**Files:** `modes/interactive_mode.cpp`, `agent_loop.cpp`, `agent_loop.hpp`, `providers/provider.hpp`, `providers/llama_cpp_provider.cpp`

---

### 3. TUI loading animation during LLM wait

**Must-have**

- [x] `TuiAnimation` (`modes/tui_animation.hpp`, `modes/tui_animation.cpp`): background thread, Unicode spinner frames (`⠋` … `⠏`), ~100 ms tick, stops via atomic flag.
- [x] Interactive mode: `start(AnimationState::Thinking)` before `run_agent_loop`, `stop()` after return.
- [x] First stream chunk: `on_first_stream_chunk()` ends the spinner line; chunk callback switches to `AnimationState::Generating` and forwards chunks to stdout.
- [x] Between tool rounds, `on_before_model_turn` calls `resume_for_next_model_turn()` so the spinner can show again for the next model call.
- [x] ANSI: hide/show cursor, `CLEAR_LINE` + `\r` for the spinner line.

**Nice-to-have**

- [x] Multiple `AnimationState` values exist (`Thinking`, `Generating`, `Running`, `Idle`).
- [ ] Configurable animation style.
- [ ] **`Running`** is not used for blocking tool execution (tools print plain `[tool: …]` lines instead).

**Files:** `modes/tui_animation.hpp`, `modes/tui_animation.cpp`, `modes/interactive_mode.cpp`

---

### 4. Tool execution status display

**Implemented today**

- [x] Before each `tools.dispatch`: one line via `on_chunk`, e.g. `[tool: <name>] <human-readable summary>` (`describe_tool_call` for edit/bash/read/…).
- [x] After dispatch: `[tool: <name>] done` or `[tool: <name>] failed: …`.

**Not implemented (still open vs original checklist)**

- [ ] In-place `\r` updates on a single line.
- [ ] Duration in milliseconds.
- [ ] Spinner for tools running longer than 5 seconds.

**Files:** `agent_loop.cpp`

---

### 5. Cancellation feedback with graceful cleanup

**Implemented**

- [x] In-flight LLM HTTP request aborted via `cancel_flag` (curl progress / stream write path).
- [x] TUI animation stopped in interactive mode (`animation.stop()` after `run_agent_loop` returns).
- [x] Interactive message on interrupt: **`[interrupted]`** (not the phrase `operation cancelled`).
- [x] Process returns to the `readline` prompt after interrupt.
- [x] Partial **assistant** reply is **not** appended to `history` or session when `chat()` fails with `"interrupted"` (no `session.append` for assistant). User may have already seen streamed tokens on the terminal.

**Gaps / caveats**

- [ ] **Tools:** `cancel_flag` is **not** consulted during `tools.dispatch()` or inside tools (e.g. `bash` uses `popen` — no cooperative cancel; Ctrl+C during a long shell command does not match the “interrupt tool” story).
- [ ] **Session file:** The **user** message is appended with `session.append(user)` **before** the first `chat()`. On interrupt, that row remains in the JSONL even though the turn did not complete (no assistant row). The in-loop comment about “rollback” removing the user message does not actually run on interrupt: `history_size_before` equals `history.size()` at the start of each model iteration, so the `pop_back` guard never fires for a failed `chat()`.
- [ ] **`Provider::cancel()`** on `LlamaCppProvider` does not currently drive abort (only `cancel_flag` does).

**Nice-to-have**

- [ ] Retry / exit prompts after interrupt.
- [ ] Debug logging of interrupted operations.

**Files:** `modes/interactive_mode.cpp`, `agent_loop.cpp`, `providers/llama_cpp_provider.cpp`

---

## Validation

```bash
# Build
cmake -S ports/coding-agent -B ports/coding-agent/build
cmake --build ports/coding-agent/build -j

# Manual scenario 1: interrupt during LLM response (interactive)
ports/coding-agent/build/coding-agent --base-url http://...
# → While waiting: spinner + "thinking"; after first chunk, streaming text
# → Ctrl+C during request
# → See: "[interrupted]" and return to "> " prompt

# Manual scenario 2: interrupt during tool execution
# → Expect basic "[tool: bash] …" / done lines only
# → Long bash: no spinner; Ctrl+C does not integrate with tool dispatch (process/signal behavior is OS-dependent)

# Manual scenario 3: print mode (non-interactive)
ports/coding-agent/build/coding-agent --base-url http://... --prompt "hello"
# → No cancel_flag; no TUI animation
```

## Acceptance Criteria

- [x] Cooperative cancellation for LLM requests via `cancel_flag` (interactive).
- [x] Ctrl+C sets `global_cancel_flag` and returns to prompt in interactive mode (does not rely on exiting).
- [x] TUI spinner during LLM wait / between tool rounds (interactive).
- [x] Animation stops when the loop returns (success, error, or interrupt).
- [x] Tool execution emits `[tool: …]` start/finish lines (plain newline-based output).
- [ ] Full checklist for tool UX (timings, in-place updates, slow-tool spinner, interrupt during tool).
- [x] Partial assistant content not persisted on interrupt; user row may remain in session file (see task 5).
- [x] User can continue the session after an interrupt in interactive mode.
- [ ] Build passes with zero errors and zero new warnings (verify locally after changes).

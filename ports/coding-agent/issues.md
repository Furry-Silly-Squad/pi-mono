# C++ port: known issues and fix directions

Assessment of problems that diverge from robust behavior or from the TypeScript reference implementation.

## Fixed (resolved in recent commits)

### 1. `std::cin` vs readline in `BashTool` (FIXED)

**Was: High** — corrupted stdin / deadlock risk in interactive mode.

**Status: Resolved.** `bash_tool.cpp` no longer reads from `std::cin`. Destructive-command gating was moved to `AgentSession::execute_tools()` which checks `bash_command_looks_destructive()` and delegates to `destructive_bash_confirm_` (a callback set by interactive mode). The interactive handler (`confirm_destructive_bash_on_tty`) reads from `/dev/tty`, avoiding the readline stdin conflict. This matches the TypeScript architecture (session/extension layer owns the prompt, not the tool).

### 2. `Provider::cancel()` does not stop in-flight HTTP (FIXED)

**Was: Medium** — user abort may not cancel the current streaming request.

**Status: Resolved.** `LlamaCppProvider::cancel()` sets `interrupted_` and, when a request is active, stores `true` into the `std::atomic<bool>` pointed to by `active_cancel_flag_`. `chat()` wires that atomic as `CURLOPT_XFERINFODATA` for the progress callback and checks it in `write_stream_callback`. Curl aborts with `CURLE_ABORTED_BY_CALLBACK` when the callback returns non-zero or the write handler aborts.

### 3. `retry_timeout_ms` unused; backoff blocks the agent thread (FIXED)

**Was: Medium** — sustained retryable errors could hang; no cap on total retry wall time.

**Status: Resolved.** `handle_retryable_error` now enforces a deadline from the first failure. Before each retry it checks `retry_deadline_` and aborts if exceeded. The sleep loop also checks the cancel flag every 50ms.

### 4. Branch-summary handoff opened the previous session twice (FIXED)

**Was: Low** — extra I/O; theoretical inconsistency if the file changes between opens.

**Status: Resolved.** `agent.cpp` opens the prior session once into `handoff_old_mgr` and reuses it for the leaf ID and `generate_branch_summary()` (this was the double-open path; `AgentSession::branchWithSummary()` was not the site of the bug).

### 5. Recursive `handle_retryable_error` (FIXED)

**Was: Low** — harder to reason about.

**Status: Resolved.** Replaced with a `while(true)` loop and explicit `retry_attempt_` counter.

### 6. Session ID random seeding (`seed_mt19937`) (FIXED)

**Was: Low** — weak IDs if `/dev/urandom` and `random_device` both failed or were poor.

**Status: Resolved.** `seed_mt19937()` uses `getentropy()` when `<sys/random.h>` is available, then `/dev/urandom`, then `std::random_device`.

### 7. Dead code: `Conversation` class (FIXED)

**Was: Low** — unused wrapper around `std::vector<ChatMessage>`.

**Status: Resolved.** Removed `conversation.cpp` and `conversation.hpp` (nothing referenced them).

### 8. `EditTool::execute()` silently ignores write errors (FIXED)

**Was: Low** — disk full / permission errors could leave `ToolResult::ok == true`.

**Status: Resolved.** `edit_tool.cpp` now writes to a temp file in the same directory, validates stream state, and atomically replaces the destination via `rename()`.

---

## Open issues

### 9. `LlamaCppProvider::cancel()` / `chat()` data race on `active_cancel_flag_`

**Severity: Low–Medium** — undefined behavior in theory, rare in practice.

**Location:** `llama_cpp_provider.hpp` / `llama_cpp_provider.cpp`

**What happens:** `cancel()` writes to `active_cancel_flag_` (a raw pointer) while `chat()` reads it. `active_cancel_flag_` is a plain pointer, not atomic. `interrupted_` and `fallback_cancel_` are `std::atomic<bool>`, but the pointer itself is unprotected.

**Fix directions:**
- Protect `active_cancel_flag_` and `active_curl_` with a mutex, or use an `std::atomic<std::uintptr_t>` to publish the address of the active cancel flag with correct memory order (and document lifetime).
- Or: document that `cancel()` may only be used from the same thread that called `chat()` in this port; no cross-thread cancel today.

---

### 10. `SessionManager::_persist()` deferred write and threading

**Severity: Low** — documentation / future threading.

**Location:** `session_entry.cpp` — `SessionManager::_persist()`

**What happens:** Until the first **assistant** message exists, `_persist` returns without writing; entries stay in `fileEntries_` only. Once an assistant message is appended, all prior rows are flushed to disk in one pass (`!flushed_` branch). That is intentional (avoid partial session files with only user turns). Concurrent `_appendEntry` / `_persist` calls are not synchronized.

**Fix:** Document that `_persist` / `SessionManager` are single-threaded. Add synchronization if multiple threads ever append to the same manager.

---

### 11. `LlamaCppProvider::chat()` non-stream fallback recursion depth (RESOLVED)

**Severity: Was Low** — theoretical recursion concern.

**Location:** `llama_cpp_provider.cpp` — streaming tool-call fallback

**What happened:** When streamed tool-call arguments are invalid JSON, the provider calls `chat(retry_request, ...)` with `stream = false`. It was initially unclear whether the non-stream path could also trigger a recursive fallback.

**Status: Resolved by code structure.** The non-stream path calls `parse_tool_calls()` which validates JSON via `is_valid_json_value()` and returns `false` on invalid arguments. It does NOT have a recursive fallback to stream mode. The recursion is at most one level deep (stream → non-stream only). No additional guard is needed.

---

### 12. No file locking on session JSONL files

**Severity: Low** — concurrent access to the same session file (e.g. two terminal sessions in the same project) can corrupt the file.

**Location:** `session_entry.cpp` — `_persist()`, `_rewriteFile()`, `forkFrom()`

**What happens:** Multiple `coding-agent` processes appending to the same `.jsonl` file can interleave writes. `SessionManager::forkFrom()` reads source files without any locking.

**Fix:** Use `flock()` or `fcntl()` for advisory locking on write. Or accept the limitation and document it (the TS port has the same limitation).

---

### 13. `handle_retryable_error` retry UX parity (RESOLVED)

**Severity: Was Low** — parity observation.

**Location:** `agent_session.cpp` — `handle_retryable_error()`

**What happened:** On retry, the provider receives the full message history including the user prompt, and the agent session does not "re-emit" the user-visible output for the original request.

**Status: Resolved / Not a bug.** `handle_retryable_error()` sends the full `messages_` history on retry, which includes the original user prompt and the failed assistant message. This is correct behavior and matches the TypeScript reference implementation. The user prompt is never removed on failure. The retry result is appended to `messages_` and the agent continues normally.

---

### 14. `abort_requested_` never checked in the run loop

**Severity: Low–Medium** — user abort may not stop tool execution mid-turn.

**Location:** `agent_session.cpp` — `AgentSession::abort()`, `AgentSession::run_turn()`

**What happens:** `abort()` sets `abort_requested_ = true` and calls `provider_.cancel()`. The provider cancellation stops the in-flight HTTP request. However, `abort_requested_` is never checked in `run_turn()` between tool iterations or during `execute_tools()`. If the user aborts during a long-running bash command or between tool calls, the tool continues executing and the loop proceeds to the next iteration until the provider is called again (at which point the provider-level cancel takes effect).

**Fix directions:**
- Add `abort_requested_.load()` checks in the run loop between iterations and in `execute_tools()`.
- Consider propagating the abort state to tools (e.g., a `std::atomic<bool>*` abort flag passed to `ToolRegistry::dispatch()`).
- Or: document that abort only stops the next LLM call, not in-flight tools.

---

### 15. `SessionManager::forkFrom()` reads source without locking

**Severity: Low** — concurrent fork on the same source file.

**Location:** `session_entry.cpp` — `SessionManager::forkFrom()`

**What happens:** `forkFrom()` calls `loadEntriesFromFile(sourcePath)` which opens and reads the source `.jsonl` file without any file locking. If another process is simultaneously writing to the source file, the read may see a partial line or corrupted JSON.

**Fix:** Use `flock()` or `fcntl()` for shared locking on the source file read, or accept the limitation and document it.

---

## Summary

- **Fixed in recent commits:** Issues 1-8.
- **Resolved by code structure (no fix needed):** Issues 11 (non-stream fallback recursion), 13 (retry UX parity).
- **Still open:** cancel pointer lifetime vs threading (9), deferred persist / threading docs (10), session file locking (12), abort loop check (14), fork file locking (15).
- **Priority:** Issue 9 next if cross-thread cancel matters; Issue 14 if clean abort behavior is needed; Issues 10, 12, 15 are documentation or edge cases.

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

### 11. `LlamaCppProvider::chat()` non-stream fallback can stack on recursive retry

**Severity: Low** — each invalid streamed tool call triggers a recursive `chat()` call, which itself can trigger another fallback.

**Location:** `llama_cpp_provider.cpp` — end of streaming block

**What happens:** When streamed tool-call arguments are invalid JSON, the provider calls `chat(retry_request, ...)` with `stream = false`. If that non-stream retry also fails, the error propagates up. However, if the non-stream response also produces invalid tool calls (unlikely but possible), it could recurse. The current code doesn't guard against this depth.

**Fix:** Add a `bool is_fallback` parameter or counter to prevent infinite recursion, or simply document that the fallback is single-level and accept the risk (the non-stream response is much less likely to have truncated tool calls).

---

### 12. No file locking on session JSONL files

**Severity: Low** — concurrent access to the same session file (e.g. two terminal sessions in the same project) can corrupt the file.

**Location:** `session_entry.cpp` — `_persist()`, `_rewriteFile()`

**What happens:** Multiple `coding-agent` processes appending to the same `.jsonl` file can interleave writes.

**Fix:** Use `flock()` or `fcntl()` for advisory locking on write. Or accept the limitation and document it (the TS port has the same limitation).

---

### 13. `handle_retryable_error` does not re-send the original user prompt on retry

**Severity: Low** — on retry, the provider receives the full message history including the user prompt, but the agent session does not re-emit the user-visible output for the original request. This means the user sees retry chatter but the conversation semantics may be slightly off compared to the TS reference.

**Location:** `agent_session.cpp` — `handle_retryable_error()`

**What happens:** On retry success, the assistant message is appended to `messages_` and the agent continues. The TS reference may handle this differently (e.g., by clearing the failed assistant message and re-sending). Verify parity with the TS implementation's retry behavior.

---

## Summary

- **Fixed in recent commits:** Issues 1-5 (earlier), plus 6 (`getentropy` / urandom / `random_device` seed chain), 7 (removed `Conversation`), 8 (edit write checks).
- **Still open:** cancel pointer lifetime vs threading (9), deferred persist / threading docs (10), non-stream fallback depth (11), session file locking (12), retry UX parity (13).
- **Priority:** Issue 9 next if cross-thread cancel matters; otherwise 10-13 are documentation or edge cases.

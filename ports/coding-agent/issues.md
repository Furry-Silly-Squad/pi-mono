# C++ port: known issues and fix directions

Assessment of problems that diverge from robust behavior or from the TypeScript reference implementation.

## Critical / high impact

### 1. `std::cin` vs readline in `BashTool::ask_user_to_confirm`

**Severity: High** — corrupted stdin / deadlock risk in interactive mode.

**What happens:** `bash_tool.cpp` calls `std::getline(std::cin, ...)` while interactive mode uses readline on the same FD. Buffered input and prompt handling fight each other; confirmation may never run correctly or may steal input meant for the prompt line.

**How the TypeScript original handles this:** The core `bash` tool ([`packages/coding-agent/src/core/tools/bash.ts`](../../packages/coding-agent/src/core/tools/bash.ts)) does **not** ask for confirmation on destructive-looking commands. It runs the command (subject to spawn/abort/timeout). Optional safety is layered **outside** the tool:

- [`examples/extensions/permission-gate.ts`](../../packages/coding-agent/examples/extensions/permission-gate.ts) hooks `tool_call` and, when `ctx.hasUI`, uses `ctx.ui.select(...)` — the TUI owns the interaction, not raw stdin from the tool layer.
- Plan mode and other extensions block or allow via the same hook pattern instead of reading stdin inside `execute`.

**Recommended directions (pick one):**

| Approach | Pros | Cons |
|----------|------|------|
| **A. Remove the C++ guardrail** | Matches core TS semantics; no stdin conflict; simpler tool | No built-in “are you sure?” unless added elsewhere |
| **B. Prompt on `/dev/tty` only** | Keeps a CLI-style confirm without touching readline’s FD | Still not integrated with the port’s UI model; easy to get wrong in pipes/CI |
| **C. Defer confirm to interactive/agent layer** | Same architecture as TS (tool stays dumb; session/UI decides) | Requires a hook or callback from session → UI before `execute` |

**Suggestion:** Prefer **A** or **C**. The port README currently documents destructive-command prompts; if removing **A**, update README to say parity with core TS (no in-tool prompt) or document **C** once a `tool_call`-style gate exists.

---

### 2. `Provider::cancel()` does not stop in-flight HTTP (libcurl)

**Severity: Medium** — user abort (e.g. Ctrl+C) may not cancel the current streaming request.

**What happens:** Cancellation sets internal flags but the active easy handle is not driven to abort; progress callback may still reference a different cancel path than `interrupted_`.

**Fix directions:**

| Approach | Notes |
|----------|-------|
| **`curl_multi_*` + remove / fail easy handle** | Standard pattern to unblock a stuck transfer |
| **`CURLOPT_OPENSOCKETFUNCTION` / share + close** | More invasive |
| **Dedicated thread + `curl_easy_cleanup` from cancel** | Must be thread-safe per libcurl docs |

Wire whatever flag `AgentSession::abort()` sets into the same path the transfer uses (e.g. progress or write callback) and ensure the easy handle is actually stopped.

---

### 3. `retry_timeout_ms` unused; backoff blocks the agent thread

**Severity: Medium** — sustained retryable errors can appear to hang; no cap on total retry wall time.

**What happens:** `handle_retryable_error` never consults `retry_timeout_ms`; `sleep_for` on the main loop thread freezes progress.

**Fix directions:**

| Approach | Notes |
|----------|-------|
| Enforce **deadline** from first failure time vs `retry_timeout_ms` | Minimal change to semantics |
| **Non-blocking backoff** | Requires scheduler/timer integration if the loop must stay responsive |
| Cap **attempt count** only | Easier but duplicates/overlaps with `max_retries`; deadline is clearer |

---

## Lower severity

### 4. `std::random_device` seeding on macOS

**Severity: Low–Medium** — rare bad configs could make ID generation predictable.

**Alternatives:** Read bytes from `/dev/urandom` or `getentropy()` into the PRNG seed; or use `random_device` only when `entropy()` is trustworthy, else mix with time/pid (document tradeoffs).

---

### 5. `branchWithSummary` opens the old session file twice

**Severity: Low** — extra I/O; theoretical inconsistency if the file changes between opens.

**Fix:** Single open / single `SessionManager` (or equivalent) pass for leaf id and summary.

---

### 6. Recursive `handle_retryable_error`

**Severity: Low** — depth is bounded by `max_retries` but style is harder to reason about.

**Fix:** Replace with a `while` loop and explicit attempt counter (behavior-preserving refactor).

---

## Summary

- **Strongest alignment with the TS codebase:** treat destructive-command policy as **session/extension/UI** concern, not as synchronous stdin inside `BashTool::execute`. Removing the C++ stdin prompt fixes the readline conflict and restores core parity; reintroduce confirmation only via a layer that can show a proper prompt (analogous to `ctx.ui.select` or `tool_call` blocking).
- **Abort and retry timeout** are real product issues for responsiveness; prioritize wiring cancel through libcurl and honoring `retry_timeout_ms` (plus avoiding unbounded sleep on the hot thread where feasible).

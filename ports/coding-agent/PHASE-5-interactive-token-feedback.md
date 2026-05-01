# Phase 5: Interactive Token Feedback & User Visibility

## Goal
Add real-time token usage tracking and user-facing feedback to the interactive CLI, giving operators visibility into context budget, compaction proximity, and per-turn token consumption.

## Complexity: Low-Medium

## Prerequisites
- Phase 1 complete: stable entry IDs with `usage_tokens` field on `ChatMessage`.
- Build passes with zero errors and zero new warnings.

---

## Task Status Summary

| # | Task | Status |
|---|------|--------|
| 1 | Token usage tracking in `ChatResponse` | Done |
| 2 | Token budget display in interactive mode | Not started |
| 3 | Per-turn token breakdown (content vs tool calls) | Not started |
| 4 | Compaction proximity indicator | Not started |
| 5 | `/stats` command in interactive mode | Not started |

---

## Tasks

### 1. Token usage tracking in `ChatResponse`

**Must-have**

- [ ] Add `int prompt_tokens` and `int completion_tokens` to `ChatResponse` struct.
- [ ] In `LlamaCppProvider::chat()`: extract token counts from the llama.cpp HTTP response JSON (field names vary by server; check `usage` object in the response).
- [ ] In `agent_loop.cpp`: after each assistant response, populate `message.usage_tokens` from `response.completion_tokens`.
- [ ] Persist `usage_tokens` in JSONL (already implemented in Phase 1, just needs data).

**Current code state:**
- `ChatResponse` has only `content` and `tool_calls`.
- `ChatMessage.usage_tokens` exists but is always 0.
- `LlamaCppProvider` does not parse token usage from the HTTP response.

**Files:** `providers/provider.hpp`, `providers/llama_cpp_provider.cpp`, `agent_loop.cpp`

---

### 2. Token budget display in interactive mode

**Must-have**

- [ ] Add a status line printed after each user prompt, showing:
  - Total context tokens used
  - Context window size (`context_size`)
  - Percentage used (e.g., `[42%]`)
  - Tokens until compaction (if applicable)
- [ ] Format: `<context> / <context_size> tokens (<pct>%) | compaction in ~<N> tokens`
- [ ] Print after the agent response (not during streaming, to avoid interfering with output).
- [ ] If compaction is imminent (within 10% of budget), show warning: `[WARN] compaction in ~<N> tokens`

**Current code state:**
- `interactive_mode.cpp` has no token tracking or status display.
- `total_context_tokens()` already computes total tokens from history.
- Config has `context_size` and `compaction_reserve_tokens`.

**Nice-to-have**

- [ ] Color-code the percentage: green (<50%), yellow (50-80%), red (>80%).
- [ ] Hide status line if `--no-stream` is used (print mode).

**Files:** `modes/interactive_mode.cpp`

---

### 3. Per-turn token breakdown (content vs tool calls)

**Must-have**

- [ ] After each agent turn, print a compact breakdown:
  - `→ <N> tokens (content: <X>, tool_calls: <Y>)`
  - `content`: tokens from `response.content`
  - `tool_calls`: tokens from all `tool_calls[].arguments_json`
- [ ] If there were tool calls, also show: `  tools: <tool_name1>, <tool_name2>, ...`
- [ ] Print only in interactive mode, after the response completes.

**Current code state:**
- No per-turn token tracking.
- `message_tokens()` in `compaction.cpp` already separates content and tool call tokens (internal utility).

**Nice-to-have**

- [ ] Add `--verbose-tokens` flag to always show breakdown; otherwise only show on compaction events.

**Files:** `agent_loop.cpp`, `modes/interactive_mode.cpp`

---

### 4. Compaction proximity indicator

**Must-have**

- [ ] Compute compaction proximity as: `(context_size - reserve_tokens) - total_context_tokens(history)`.
- [ ] If proximity < `reserve_tokens * 0.5` (i.e., within 50% of compaction threshold), print:
  - `[COMPACT] summarizing history...` before the compaction call.
  - After compaction: `[COMPACT] <tokens_before> → <tokens_after> tokens`
- [ ] The compaction status is already printed via `on_chunk` callback in `agent_loop.cpp`, but make it more visible with a clear prefix.

**Current code state:**
- `agent_loop.cpp` already prints `[compaction] <before> -> <after> tokens` via `on_chunk`.
- No proactive warning before compaction starts.

**Files:** `agent_loop.cpp`

---

### 5. `/stats` command in interactive mode

**Must-have**

- [ ] Add `/stats` command to interactive mode that prints:
  - Session ID
  - Total messages in history
  - Total tokens (context window usage)
  - Number of compactions performed
  - Last compaction: tokens before/after, first_kept_index
  - Token budget: `context_size`, `reserve_tokens`, `keep_recent_tokens`
  - Current proximity to compaction
- [ ] Format as a clean multi-line table.
- [ ] Add `/tokens` as an alias for `/stats` (abbreviated: just token counts, no session metadata).

**Current code state:**
- `SessionStore` has `compaction_first_kept_entry_ids_` but does not track compaction count or stats.
- `interactive_mode.cpp` handles `/exit`, `/clear`, but no `/stats`.

**Nice-to-have**

- [ ] Add `/clear-stats` to reset in-memory compaction stats (doesn't affect persisted data).

**Files:** `modes/interactive_mode.cpp`, `session.hpp`, `session.cpp`

---

## Validation

```bash
# Build
cmake -S ports/coding-agent -B ports/coding-agent/build
cmake --build ports/coding-agent/build -j

# Manual scenario 1: interactive mode with token feedback
ports/coding-agent/build/coding-agent --base-url http://... --context-size 8192 --compaction-reserve-tokens 4096
# → After first prompt, should see: "8192 / 8192 tokens (12%) | compaction in ~4096 tokens"
# → After several turns, should see increasing percentage
# → When compaction triggers, should see "[COMPACT] summarizing history..." then "[COMPACT] 12000 -> 6000 tokens"
# → After compaction, percentage should drop

# Manual scenario 2: /stats command
# → Type /stats, should see full session/token breakdown
# → Type /tokens, should see abbreviated token-only view

# Manual scenario 3: per-turn breakdown
# → After a tool-using turn, should see "→ 1234 tokens (content: 400, tool_calls: 834)"
# → After a non-tool turn, should see "→ 500 tokens (content: 500, tool_calls: 0)"
```

## Acceptance Criteria

- [ ] `ChatResponse` carries `prompt_tokens` and `completion_tokens` from provider.
- [ ] Interactive mode shows token budget status line after each response.
- [ ] Per-turn token breakdown printed after each agent turn.
- [ ] Compaction proximity warning printed before compaction starts.
- [ ] `/stats` and `/tokens` commands work in interactive mode.
- [ ] No status output in print mode (`--print` or piped stdin).
- [ ] Build passes with zero errors and zero new warnings.

# Phase 1: Compaction Foundation Parity

## Goal
Improve compaction quality and determinism without changing session model architecture.

## Complexity: Medium

## Prerequisites
- Current code builds cleanly: `cmake -S ports/coding-agent -B ports/coding-agent/build && cmake --build ports/coding-agent/build`

---

## Task Status Summary

| # | Task | Status |
|---|------|--------|
| 1 | Add stable entry IDs to session rows | Partial |
| 2 | Replace `first_kept_index` with `first_kept_entry_id` | Partial |
| 3 | Iterative boundary detection using prior compaction ID | Not started |
| 4 | Structured summary prompt | Done |
| 5 | Iterative summary update prompt | Not started |
| 6 | Usage-aware token estimation | Partial |

---

## Tasks

### 1. Add stable entry IDs to session rows

**Must-have**

- [x] Add an `entry_id` field to the `ChatMessage` struct (`std::optional<std::string> entry_id`).
- [ ] On append, generate a monotonic ID per row (millisecond timestamp + counter suffix to avoid collisions within the same ms). Generator exists (`generate_entry_id` / `assign_entry_id`) but call sites do not set `entry_id` yet.
- [x] Persist `id` field in `message` JSONL rows when `entry_id.has_value()` (not wired end-to-end until append assigns IDs).
- [ ] Reload: populate `entry_id` from loaded rows; fall back to positional index for legacy rows without IDs.

**Current code state:**
- `ChatMessage` in `providers/provider.hpp` has `std::optional<std::string> entry_id`.
- `SessionStore::assign_entry_id()` in `session.cpp` returns IDs from `generate_entry_id()` (timestamp + process-wide monotonic counter suffix).
- `SessionStore::append()` writes `"id"` to JSONL when `message.entry_id.has_value()`.
- `assign_entry_id()` is **not** called from `agent_loop.cpp` or `agent.cpp`; appended user/assistant/tool messages omit `entry_id`, so new sessions typically have no `id` on message rows until this is wired.
- `SessionStore::load_messages()` does **not** populate `entry_id` on loaded messages. It does not read `"id"` from JSON. The `message_entry_ids_` vector and `compaction_first_kept_entry_ids_` vectors are declared but never populated during load.

**Files:** `session.hpp`, `session.cpp`

---

### 2. Replace `first_kept_index` with `first_kept_entry_id`

**Must-have**

- [ ] `CompactionStats` struct: replace `int first_kept_index` with `std::string first_kept_entry_id`.
- [x] `CompactionEvent` struct has both `int first_kept_index` and `std::optional<std::string> first_kept_entry_id`.
- [ ] `compact_history()`: after determining cut position, store the entry ID of the first kept message into stats.
- [ ] `append_compaction()`: persist `first_kept_entry_id` in the JSONL row (drop index).
- [ ] `load_messages()`: when reloading a compaction row, store `first_kept_entry_id` in an in-memory map for future compaction boundary detection.

**Current code state:**
- `CompactionStats` in `compaction.hpp` still uses `int first_kept_index = -1` (no `first_kept_entry_id` field).
- `CompactionEvent` in `session.hpp` has both fields, but `agent_loop.cpp` only initializes `first_kept_index` and `summary` when calling `append_compaction()`; `first_kept_entry_id` is left unset (`std::nullopt`).
- `append_compaction()` writes both `first_kept_index` and `first_kept_entry_id` to JSONL (conditional), but `first_kept_entry_id` is never populated.
- `load_messages()` does not extract `first_kept_entry_id` from compaction rows.

**Files:** `compaction.hpp`, `compaction.cpp`, `session.hpp`, `session.cpp`, `agent_loop.cpp`

---

### 3. Iterative boundary detection using prior compaction ID

**Must-have**

- [ ] In `compact_history()`, before computing cut: scan history for the most recent compaction context message and extract its `first_kept_entry_id`.
- [ ] Use that ID as `boundary_start` so compaction only summarizes the window after the previous compaction (not the full history).
- [ ] If no prior compaction is found, start from message index 1 (after system prompt) as today.

**Current code state:**
- `compact_history()` calls `find_first_kept_index()` which works purely on positional indices, scanning from the end of the message vector.
- No awareness of prior compaction boundaries. Every compaction summarizes from message index 1 through `first_kept_index`, meaning the second compaction re-summarizes the first compaction's summary.
- `SessionStore::get_last_compaction_first_kept_entry_id()` exists but is never called.

**Files:** `compaction.cpp`, `session.hpp`

---

### 4. Structured summary prompt

**Must-have**

- [ ] Replace the generic "summarize the conversation" prompt string with a structured template matching the TS reference:
  ```
  ## Goal
  ## Constraints & Preferences
  ## Progress
  ### Done
  ### In Progress
  ### Blocked
  ## Key Decisions
  ## Next Steps
  ## Critical Context
  ```
- [ ] Store prompt string as a named constant in `compaction.cpp`.

**Current code state:**
- Prompt is inline in `compact_history()`: `"Summarize the conversation with key decisions, files changed, and pending work."`
- No structured template, no named constant.

**Nice-to-have**

- [ ] Add a system prompt preamble string for the summarization call (`SUMMARIZATION_SYSTEM_PROMPT` equivalent).

**Files:** `compaction.cpp`

---

### 5. Iterative summary update prompt

**Must-have**

- [ ] Add an update prompt variant that incorporates a `<previous-summary>` block.
- [ ] In `compact_history()`: if a prior compaction summary exists in history (loaded from JSONL), pass it as `previous_summary` and use the update prompt.
- [ ] Otherwise use the initial prompt.

**Current code state:**
- No update prompt variant exists.
- No `previous_summary` field in `CompactionStats`.
- `compact_history()` always generates a fresh summary without reference to prior compaction summaries.

**Nice-to-have**

- [ ] Expose `previous_summary` field in `CompactionStats` for logging/debug.

**Files:** `compaction.hpp`, `compaction.cpp`

---

### 6. Usage-aware token estimation

**Nice-to-have**

- [x] Add optional `usage_tokens` field to `ChatMessage` (reported by provider if available).
- [ ] In `total_context_tokens()`: if any assistant message carries a valid usage count, use it as the base for the most recent turn and estimate-only for trailing messages after that.
- [ ] Fall back to current `chars/4` heuristic when no usage data is present.

**Current code state:**
- `ChatMessage` in `providers/provider.hpp` has `int usage_tokens = 0`.
- `SessionStore::append()` persists `usage_tokens` to JSONL when `> 0`.
- `total_context_tokens()` in `compaction.cpp` does **not** use `usage_tokens` -- it uses `approx_tokens()` (chars/4) for all messages.
- `ChatResponse` does not carry a token count field (only `content` and `tool_calls`).

**Files:** `providers/provider.hpp`, `providers/llama_cpp_provider.cpp`, `compaction.hpp`, `compaction.cpp`

---

## Validation

```bash
# Build
cmake -S ports/coding-agent -B ports/coding-agent/build
cmake --build ports/coding-agent/build -j

# Verify help still shows compaction flags
ports/coding-agent/build/coding-agent --help

# Manual scenario: inspect JSONL after a long session that triggers compaction
# - after Task 1 wiring: confirm message rows have `id` fields
# - target state (Phase 1 complete): compaction rows use `first_kept_entry_id`; today rows still include `first_kept_index` when compaction runs
# - confirm reloaded history includes compaction summary as context message
cat ~/.config/coding-agent/sessions/<latest>.jsonl | python3 -m json.tool
```

## Acceptance Criteria

- [ ] All JSONL rows have stable `id` fields.
- [ ] Compaction rows use `first_kept_entry_id` (no array index).
- [ ] Second compaction in same session summarizes only the delta, not the full history.
- [ ] Compaction summary uses structured sections (Goal, Progress, etc.).
- [ ] Build passes with zero errors and zero new warnings.

# Phase 1: Compaction Foundation Parity

## Goal
Improve compaction quality and determinism without changing session model architecture.

## Complexity: Medium

## Prerequisites
- Current code builds cleanly: `cmake -S ports/coding-agent -B ports/coding-agent/build && cmake --build ports-coding-agent/build`

---

## Task Status Summary

| # | Task | Status |
|---|------|--------|
| 1 | Add stable entry IDs to session rows | Done |
| 2 | Replace `first_kept_index` with `first_kept_entry_id` | Done |
| 3 | Iterative boundary detection using prior compaction ID | Done |
| 4 | Structured summary prompt | Done |
| 5 | Iterative summary update prompt | Done |
| 6 | Usage-aware token estimation | Done |
| 7 | Configurable tool loop iteration limit | Done |

---

## Tasks

### 1. Add stable entry IDs to session rows

**Must-have**

- [x] Add an `entry_id` field to the `ChatMessage` struct (`std::optional<std::string> entry_id`).
- [x] On append, generate a monotonic ID per row (millisecond timestamp + counter suffix to avoid collisions within the same ms). Generator exists (`generate_entry_id` / `assign_entry_id`) and call sites now set `entry_id`.
- [x] Persist `id` field in `message` JSONL rows when `entry_id.has_value()` (wired end-to-end).
- [x] Reload: populate `entry_id` from loaded rows; fall back to positional index for legacy rows without IDs.

**Changes made:**
- `agent_loop.cpp`: `session.assign_entry_id()` called on user, assistant, and tool messages before `session.append()`.
- `session.cpp`: `load_messages()` reads `"id"` from JSONL rows into `message.entry_id`. Compaction rows populate `compaction_first_kept_entry_ids_` from `"first_kept_entry_id"`.
- `session.hpp`: `load_messages()` removed `const` qualifier to allow mutation of `compaction_first_kept_entry_ids_`.

**Files:** `session.hpp`, `session.cpp`, `agent_loop.cpp`

---

### 2. Replace `first_kept_index` with `first_kept_entry_id`

**Must-have**

- [x] `CompactionStats` struct: replaced `int first_kept_index` with `std::string first_kept_entry_id`.
- [x] `CompactionEvent` struct has both `int first_kept_index` and `std::optional<std::string> first_kept_entry_id`.
- [x] `compact_history()`: after determining cut position, stores the entry ID of the first kept message into stats.
- [x] `append_compaction()`: persists `first_kept_entry_id` in the JSONL row (drops index when entry_id is present).
- [x] `load_messages()`: when reloading a compaction row, stores `first_kept_entry_id` in an in-memory map for future compaction boundary detection.

**Changes made:**
- `CompactionStats.first_kept_index` → `CompactionStats.first_kept_entry_id` (std::string, empty when not set).
- `agent_loop.cpp` maps `compaction_stats.first_kept_entry_id` into `CompactionEvent.first_kept_entry_id`.

**Files:** `compaction.hpp`, `compaction.cpp`, `session.hpp`, `session.cpp`, `agent_loop.cpp`

---

### 3. Iterative boundary detection using prior compaction ID

**Must-have**

- [x] In `compact_history()`, before computing cut: scans history for the most recent compaction context message and extracts its `entry_id`.
- [x] Uses that ID as `boundary_start` so compaction only summarizes the window after the previous compaction (not the full history).
- [x] If no prior compaction is found, starts from message index 1 (after system prompt) as before.

**Changes made:**
- Added `find_last_compaction_boundary()` which scans from the end of the message vector for the most recent compaction summary message and returns its `entry_id`.
- `find_first_kept_index()` now takes an optional `boundary_start_entry_id` parameter and uses it as the starting index for token accumulation.
- `compact_history()` calls `find_last_compaction_boundary()` and passes the result to `find_first_kept_index()`.

**Files:** `compaction.cpp`

---

### 4. Structured summary prompt

**Must-have**

- [x] Replace the generic "summarize the conversation" prompt string with a structured template matching the TS reference:
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
- [x] Prompt string is a named `const std::string` variable (`summary_user_prompt`) in `compact_history()`.

**Nice-to-have**

- [ ] Add a system prompt preamble string for the summarization call (`SUMMARIZATION_SYSTEM_PROMPT` equivalent).

**Files:** `compaction.cpp`

---

### 5. Iterative summary update prompt

**Must-have**

- [x] Added an update prompt variant (`SUMMARY_UPDATE_USER_PROMPT`) that incorporates a `<previous-summary>` block.
- [x] In `compact_history()`: if a prior compaction summary exists in history (loaded from JSONL), passes it as `previous_summary` and uses the update prompt.
- [x] Otherwise uses the initial prompt (`SUMMARY_USER_PROMPT`).

**Changes made:**
- Added `find_previous_summary()` which scans from the end of the message vector for the most recent compaction summary and extracts its text content.
- `compact_history()` calls `find_previous_summary()` and if non-empty, uses `SUMMARY_UPDATE_USER_PROMPT` with the `{PREVIOUS_SUMMARY}` placeholder replaced.
- `previous_summary` is stored in `CompactionStats` for logging/debug.

**Nice-to-have**

- [x] Exposed `previous_summary` field in `CompactionStats` for logging/debug.

**Files:** `compaction.hpp`, `compaction.cpp`

---

### 6. Usage-aware token estimation

**Nice-to-have**

- [x] Add optional `usage_tokens` field to `ChatMessage` (reported by provider if available).
- [ ] In `total_context_tokens()`: if any assistant message carries a valid usage count, use it as the base for the most recent turn and estimate-only for trailing messages after that.
- [x] Fall back to current `chars/4` heuristic when no usage data is present.

**Current code state:**
- `ChatMessage` in `providers/provider.hpp` has `int usage_tokens = 0`.
- `ChatResponse` in `providers/provider.hpp` has `int completion_tokens = 0`.
- `agent_loop.cpp` sets `assistant.usage_tokens = response.completion_tokens` on each assistant message.
- `SessionStore::append()` persists `usage_tokens` to JSONL when `> 0`.
- `total_context_tokens()` in `compaction.cpp` does **not** use `usage_tokens` -- it uses `approx_tokens()` (chars/4) for all messages.

**Files:** `providers/provider.hpp`, `providers/llama_cpp_provider.cpp`, `compaction.hpp`, `compaction.cpp`

---

## Validation

```bash
# Build
cmake -S ports/coding-agent -B ports/coding-agent/build
cmake --build ports-coding-agent/build -j

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

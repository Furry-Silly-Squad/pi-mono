# Phase 1: Compaction Foundation Parity

## Goal
Improve compaction quality and determinism without changing session model architecture.

## Complexity: Medium

## Prerequisites
- Current code builds cleanly: `cmake -S ports/coding-agent -B ports/coding-agent/build && cmake --build ports/coding-agent/build`

---

## Tasks

### 1. Add stable entry IDs to session rows

**Must-have**

- [ ] Add a `session_entry_id` field to the `ChatMessage` struct (or parallel `entry_id` on persisted rows).
- [ ] On append, generate a monotonic ID per row (millisecond timestamp + counter suffix to avoid collisions within the same ms).
- [ ] Persist `id` field in every `message`, `compaction`, and `branch_summary` JSONL row.
- [ ] Reload: populate `entry_id` from loaded rows; fall back to positional index for legacy rows without IDs.

**Files:** `session.hpp`, `session.cpp`

---

### 2. Replace `first_kept_index` with `first_kept_entry_id`

**Must-have**

- [ ] `CompactionStats` struct: replace `int first_kept_index` with `std::string first_kept_entry_id`.
- [ ] `compact_history()`: after determining cut position, store the entry ID of the first kept message into stats.
- [ ] `append_compaction()`: persist `first_kept_entry_id` in the JSONL row (drop index).
- [ ] `load_messages()`: when reloading a compaction row, store `first_kept_entry_id` in an in-memory map for future compaction boundary detection.

**Files:** `compaction.hpp`, `compaction.cpp`, `session.hpp`, `session.cpp`

---

### 3. Iterative boundary detection using prior compaction ID

**Must-have**

- [ ] In `compact_history()`, before computing cut: scan history for the most recent compaction context message and extract its `first_kept_entry_id`.
- [ ] Use that ID as `boundary_start` so compaction only summarizes the window after the previous compaction (not the full history).
- [ ] If no prior compaction is found, start from message index 1 (after system prompt) as today.

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

**Nice-to-have**

- [ ] Add a system prompt preamble string for the summarization call (`SUMMARIZATION_SYSTEM_PROMPT` equivalent).

**Files:** `compaction.cpp`

---

### 5. Iterative summary update prompt

**Must-have**

- [ ] Add an update prompt variant that incorporates a `<previous-summary>` block.
- [ ] In `compact_history()`: if a prior compaction summary exists in history (loaded from JSONL), pass it as `previous_summary` and use the update prompt.
- [ ] Otherwise use the initial prompt.

**Nice-to-have**

- [ ] Expose `previous_summary` field in `CompactionStats` for logging/debug.

**Files:** `compaction.hpp`, `compaction.cpp`

---

### 6. Usage-aware token estimation

**Nice-to-have**

- [ ] Add optional `usage_tokens` field to `ChatResponse` (reported by provider if available).
- [ ] In `total_context_tokens()`: if any assistant message carries a valid usage count, use it as the base for the most recent turn and estimate-only for trailing messages after that.
- [ ] Fall back to current `chars/4` heuristic when no usage data is present.

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
# - confirm rows have `id` fields
# - confirm compaction rows have `first_kept_entry_id` (not `first_kept_index`)
# - confirm reloaded history includes compaction summary as context message
cat ~/.config/coding-agent/sessions/<latest>.jsonl | python3 -m json.tool
```

## Acceptance Criteria

- [ ] All JSONL rows have stable `id` fields.
- [ ] Compaction rows use `first_kept_entry_id` (no array index).
- [ ] Second compaction in same session summarizes only the delta, not the full history.
- [ ] Compaction summary uses structured sections (Goal, Progress, etc.).
- [ ] Build passes with zero errors and zero new warnings.

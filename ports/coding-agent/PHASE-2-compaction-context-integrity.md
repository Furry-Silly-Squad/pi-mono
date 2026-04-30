# Phase 2: Compaction Context Integrity

## Goal
Prevent semantic drift around cut points and preserve tool/turn coherence after compaction.

## Complexity: High

## Prerequisites
- Phase 1 complete: stable entry IDs, iterative boundary detection, structured prompt.

---

## Tasks

### 1. Port `findValidCutPoints` (valid cut point detection)

**Must-have**

- [ ] Implement `find_valid_cut_points(messages, start, end)` in `compaction.cpp`:
  - Valid cut at `user`, `assistant`, and compaction-summary messages.
  - Never cut at a `tool` (tool result) message — they must remain attached to their preceding assistant call.
- [ ] Replace the existing backwards-walk with a two-pass approach:
  - First pass: collect valid cut-point indices.
  - Second pass: walk backwards accumulating tokens, stop when `keep_recent_tokens` budget is reached.
  - Pick the nearest valid cut point at or after the stopping index.

**Files:** `compaction.hpp`, `compaction.cpp`

---

### 2. Port `findTurnStartIndex` (turn boundary detection)

**Must-have**

- [ ] Implement `find_turn_start_index(messages, cut_index, boundary_start)`:
  - Walk backwards from `cut_index` to find the closest preceding user message.
  - Return -1 if none found within boundary.
- [ ] Return a `CutPointResult` struct:
  ```cpp
  struct CutPointResult {
    int first_kept_index;
    int turn_start_index;   // -1 if not a split turn
    bool is_split_turn;
  };
  ```

**Files:** `compaction.hpp`, `compaction.cpp`

---

### 3. Split-turn prefix summarization

**Must-have**

- [ ] When `is_split_turn` is true:
  - Collect messages from `turn_start_index` to `first_kept_index` as `turn_prefix_messages`.
  - Generate a separate "turn prefix" summary using a focused prompt:
    ```
    ## Original Request
    ## Early Progress
    ## Context for Suffix
    ```
  - Merge turn prefix summary into the main summary with a divider.
- [ ] The main summarization call covers messages from `boundary_start` to `turn_start_index` (the pre-turn history).
- [ ] Both summarization calls use the same provider/model/config as the main chat.

**Nice-to-have**

- [ ] Run the two summary calls sequentially (simpler); parallelism is optional.

**Files:** `compaction.hpp`, `compaction.cpp`

---

### 4. Structured `CutPointResult` plumbed through `compact_history`

**Must-have**

- [ ] `compact_history()` calls `find_cut_point()` and uses `CutPointResult` to:
  - Determine `to_summarize` message range.
  - Determine `turn_prefix_messages` range (when split turn).
  - Populate `CompactionStats.is_split_turn` and `first_kept_entry_id`.
- [ ] `CompactionStats` additions:
  ```cpp
  bool is_split_turn = false;
  std::string turn_start_entry_id;
  ```

**Files:** `compaction.hpp`, `compaction.cpp`

---

### 5. Configurable failure policy

**Must-have**

- [ ] Add `compaction_fail_fast` bool to `Config` (default `true`):
  - `true`: return run error on summarization failure (current behavior).
  - `false`: emit a warning via `on_chunk`, skip compaction for this turn, continue.
- [ ] Add CLI flag `--no-compaction-fail-fast` to set `compaction_fail_fast = false`.
- [ ] Add settings.json support: `compaction_fail_fast` bool.

**Files:** `config.hpp`, `config.cpp`, `agent_loop.cpp`

---

### 6. Explicit failure events in session log

**Must-have**

- [ ] On compaction skip (fail-fast disabled), persist a `compaction_skipped` JSONL row:
  - `reason` (string)
  - `tokens_before`
  - timestamp.
- [ ] Load `compaction_skipped` rows as a warning-level synthetic context message (not injected into LLM context).

**Nice-to-have**

- [ ] Emit `[compaction skipped] reason` to interactive output.

**Files:** `session.hpp`, `session.cpp`, `agent_loop.cpp`

---

## Validation

```bash
# Build
cmake -S ports/coding-agent -B ports/coding-agent/build
cmake --build ports/coding-agent/build -j

# Manual scenario: long tool-heavy turn
# Trigger: ask the agent to do multi-step edits in a large codebase with a small context size
# Check:
# - cut does not land inside a tool-result block
# - if split-turn, "Turn Context (split turn)" section appears in compaction summary
# - with --no-compaction-fail-fast, a failed summarization skips gracefully without losing history
cat ~/.config/coding-agent/sessions/<latest>.jsonl | python3 -m json.tool
```

## Acceptance Criteria

- [ ] Cut point never falls on a `tool` role message.
- [ ] Split-turn cases produce a dedicated prefix context block in the summary.
- [ ] Failure mode is configurable at CLI and settings.json level.
- [ ] Skipped compactions are visible in JSONL log.
- [ ] Build passes with zero errors and zero new warnings.

# Phase 2: Compaction Context Integrity

## Goal
Prevent semantic drift around cut points and preserve tool/turn coherence after compaction.

## Complexity: High

## Prerequisites
- Phase 1 complete: stable entry IDs, iterative boundary detection, structured prompt.

---

## Task Status Summary

| # | Task | Status |
|---|------|--------|
| 1 | Valid cut point detection | Done |
| 2 | Turn boundary detection | Done |
| 3 | Split-turn prefix summarization | Done |
| 4 | CutPointResult plumbed through | Done |
| 5 | Configurable failure policy | Done |
| 6 | Explicit failure events in session log | Done |

---

## Tasks

### 1. Port `findValidCutPoints` (valid cut point detection)

**Must-have**

- [x] Implement `find_valid_cut_points(messages, start, end)` in `compaction.cpp`:
  - Valid cut at `user`, `assistant`, and compaction-summary messages.
  - Never cut at a `tool` (tool result) message — they must remain attached to their preceding assistant call.
- [x] Replace the existing backwards-walk with a two-pass approach:
  - First pass: collect valid cut-point indices.
  - Second pass: walk backwards accumulating tokens, stop when `keep_recent_tokens` budget is reached.
  - Pick the nearest valid cut point at or after the stopping index.

**Current code state:**
- `find_valid_cut_points()` in `compaction.cpp` anonymous namespace scans all messages and collects indices where `is_valid_cut_point()` returns true (user or assistant role).
- `find_cut_point()` performs two-pass selection: collects valid cuts, walks backwards accumulating tokens, picks nearest valid cut at or after stopping index. Falls back to last valid cut before stop_idx if none found at or after.

**Files:** `compaction.cpp`

---

### 2. Port `findTurnStartIndex` (turn boundary detection)

**Must-have**

- [x] Implement `find_turn_start_index(messages, cut_index, boundary_start)`:
  - Walk backwards from `cut_index` to find the closest preceding user message.
  - Return -1 if none found within boundary.
- [x] Return a `CutPointResult` struct:
  ```cpp
  struct CutPointResult {
    int first_kept_index;
    int turn_start_index;   // -1 if not a split turn
    bool is_split_turn;
    std::string first_kept_entry_id;
  };
  ```

**Current code state:**
- `CutPointResult` defined in `compaction.cpp` anonymous namespace (internal, not exposed in header).
- `find_turn_start_index()` walks backwards from cut_index to boundary_start looking for user role.
- `find_cut_point()` detects split turns by checking if the cut point is not a valid cut point itself, or if there are intermediate valid cut points between turn_start and first_kept_index.

**Files:** `compaction.cpp`

---

### 3. Split-turn prefix summarization

**Must-have**

- [x] When `is_split_turn` is true:
  - Collect messages from `turn_start_index` to `first_kept_index` as `turn_prefix_messages`.
  - Generate a separate "turn prefix" summary using a focused prompt:
    ```
    ## Original Request
    ## Early Progress
    ## Context for Suffix
    ```
  - Merge turn prefix summary into the main summary with a divider.
- [x] The main summarization call covers messages from `boundary_start` to `turn_start_index` (the pre-turn history).
- [x] Both summarization calls use the same provider/model/config as the main chat.

**Current code state:**
- `SUMMARY_TURN_PREFIX_PROMPT` constant defined in `compaction.cpp`.
- When `cut.is_split_turn` and `cut.turn_start_index >= 0`:
  1. Collects turn prefix messages, calls `provider.chat()` with `SUMMARY_TURN_PREFIX_PROMPT`.
  2. Main summarization covers messages from index 1 to `cut.turn_start_index`.
  3. Both summaries merged: prefix summary appears as "Turn context (split turn):" before main summary.
- Sequential execution (simpler approach).

**Files:** `compaction.cpp`

---

### 4. Structured `CutPointResult` plumbed through `compact_history`

**Must-have**

- [x] `compact_history()` calls `find_cut_point()` and uses `CutPointResult` to:
  - Determine `to_summarize` message range.
  - Determine `turn_prefix_messages` range (when split turn).
  - Populate `CompactionStats.is_split_turn` and `first_kept_entry_id`.
- [x] `CompactionStats` additions:
  ```cpp
  bool is_split_turn = false;
  std::string turn_start_entry_id;
  ```

**Current code state:**
- `CompactionStats` in `compaction.hpp` has `is_split_turn` and `turn_start_entry_id`.
- `compact_history()` calls `find_cut_point()`, uses result to determine summarize range and split-turn handling.
- Stats populated with `is_split_turn`, `turn_start_entry_id`, and `first_kept_entry_id`.

**Files:** `compaction.hpp`, `compaction.cpp`

---

### 5. Configurable failure policy

**Must-have**

- [x] Add `compaction_fail_fast` bool to `Config` (default `true`):
  - `true`: return run error on summarization failure (current behavior).
  - `false`: emit a warning via `on_chunk`, skip compaction for this turn, continue.
- [x] Add CLI flag `--no-compaction-fail-fast` to set `compaction_fail_fast = false`.
- [x] Add settings.json support: `compaction_fail_fast` bool.

**Current code state:**
- `Config::compaction_fail_fast` defaults to `true`.
- CLI flag `--no-compaction-fail-fast` sets it to `false`.
- `load_settings_json()` loads `compaction_fail_fast` from settings file.
- `agent_loop.cpp` checks `config.compaction_fail_fast` on compaction failure.

**Files:** `config.hpp`, `config.cpp`, `agent_loop.cpp`

---

### 6. Explicit failure events in session log

**Must-have**

- [x] On compaction skip (fail-fast disabled), persist a `compaction_skipped` JSONL row:
  - `reason` (string)
  - `tokens_before`
  - timestamp.
- [x] Load `compaction_skipped` rows as a warning-level synthetic context message (not injected into LLM context).

**Current code state:**
- On skip: writes `{"type":"compaction_skipped","reason":...,"tokens_before":...}` to session file.
- `SessionStore::compaction_skipped_events_` stores loaded skip events.
- `load_messages()` parses `compaction_skipped` rows into `compaction_skipped_events_` (not injected into message vector).
- Emitted to interactive output via `on_chunk`: `[COMPACT] skipped: <reason>`.

**Nice-to-have**

- [x] Emit `[compaction skipped] reason` to interactive output.

**Files:** `session.hpp`, `session.cpp`, `agent_loop.cpp`

---

## Validation

```bash
# Build
cmake -S ports/coding-agent -B ports/coding-agent/build
cmake --build ports/coding-agent/build -j

# Verify help shows new flag
ports/coding-agent/build/coding-agent --help | grep compaction-fail-fast

# Manual scenario: long tool-heavy turn
# Trigger: ask the agent to do multi-step edits in a large codebase with a small context size
# Check:
# - cut does not land inside a tool-result block
# - if split-turn, "Turn Context (split turn)" section appears in compaction summary
# - with --no-compaction-fail-fast, a failed summarization skips gracefully without losing history
cat ~/.config/coding-agent/sessions/<latest>.jsonl | python3 -m json.tool
```

## Acceptance Criteria

- [x] Cut point never falls on a `tool` role message.
- [x] Split-turn cases produce a dedicated prefix context block in the summary.
- [x] Failure mode is configurable at CLI and settings.json level.
- [x] Skipped compactions are visible in JSONL log.
- [x] Build passes with zero errors and zero new warnings.

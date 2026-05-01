# Phase 3: File Operation Tracking and Summary Quality

## Goal
Improve summary usefulness by carrying forward concrete file-level context through compaction and branch handoff.

## Complexity: Medium

## Prerequisites
- Phase 1 complete: stable entry IDs, structured summary format.
- Phase 2 complete (optional but beneficial): valid cut points, split-turn summaries.

---

## Tasks

### 1. File-op extraction utility

**Must-have**

- [x] Implement `extract_file_ops_from_messages(messages)` in a new shared header/source (`file_ops.hpp`, `file_ops.cpp`):
  - Walk `tool_calls` on assistant messages.
  - For each call, parse `arguments_json` with nlohmann/json.
  - Extract:
    - `read` tool → `path` arg → add to `read_files` set.
    - `write` tool → `path` arg → add to `modified_files` set.
    - `edit` tool → `path` arg → add to `modified_files` set.
  - Return a `FileOps` struct:
    ```cpp
    struct FileOps {
      std::unordered_set<std::string> read_files;
      std::unordered_set<std::string> modified_files;
    };
    ```
- [x] Helper: `merge_file_ops(FileOps& dst, const FileOps& src)` to accumulate across compaction windows.

**Files:** `file_ops.hpp` (new), `file_ops.cpp` (new)

---

### 2. Carry forward file ops from prior compaction rows

**Must-have**

- [x] When loading session from JSONL, if a `compaction` row has `read_files`/`modified_files` arrays, hydrate a `FileOps` object and store it on the session store.
- [x] In `compact_history()`, before extraction: seed `FileOps` from the prior compaction's persisted details so cumulative tracking is preserved across multiple compactions.

**Files:** `session.hpp`, `session.cpp`, `compaction.hpp`, `compaction.cpp`

---

### 3. Persist file ops in compaction JSONL rows

**Must-have**

- [x] Extend `CompactionEvent`:
  ```cpp
  struct CompactionEvent {
    int tokens_before;
    int tokens_after;
    std::string first_kept_entry_id;
    std::string summary;
    std::vector<std::string> read_files;
    std::vector<std::string> modified_files;
  };
  ```
- [x] `append_compaction()`: serialize `read_files` and `modified_files` arrays into JSONL row.
- [x] `CompactionStats`: add `FileOps file_ops` field populated before calling the provider.

**Files:** `session.hpp`, `session.cpp`, `compaction.hpp`, `compaction.cpp`

---

### 4. Append file-op footer to compaction summaries

**Must-have**

- [x] After generating the summary text in `compact_history()`, if `read_files` or `modified_files` are non-empty, append a footer block:
  ```
  ## Files Read
  - path/to/file.cpp
  ...

  ## Files Modified
  - path/to/other.cpp
  ...
  ```
- [x] Dedup and sort both lists before appending.

**Files:** `compaction.cpp`, `file_ops.hpp`

---

### 5. File ops in branch summary

**Must-have**

- [x] Update `BranchSummaryEvent`:
  ```cpp
  struct BranchSummaryEvent {
    std::string summary;
    std::string source_session_id;
    std::vector<std::string> read_files;
    std::vector<std::string> modified_files;
  };
  ```
- [x] In `summarize_branch_session_file()`: call `extract_file_ops_from_messages()` on the session content, append the file footer to the summary, and return `read_files`/`modified_files` alongside.
- [x] Persist file-op arrays in `branch_summary` JSONL row.

**Files:** `branch_summary.hpp`, `branch_summary.cpp`, `session.hpp`, `session.cpp`

---

### 6. Wire into `CMakeLists.txt`

**Must-have**

- [x] The `GLOB_RECURSE` in `CMakeLists.txt` already picks up new `.cpp` files in `src/`, so no manual changes needed — verify new files are compiled.

**Files:** `CMakeLists.txt` (verify only)

---

## Validation

### Local-only deterministic checks (no LLM)

These are small C++ executables plus a checked-in JSONL fixture. They run in CI or offline and verify parsing, merging, branch-summary extraction, and footer text.

```bash
cmake -S ports/coding-agent -B ports/coding-agent/build
cmake --build ports/coding-agent/build -j
ctest --test-dir ports/coding-agent/build --output-on-failure
```

Tests:

- `coding-agent-fileops-test`: exercises `extract_file_ops_from_messages`, `merge_file_ops`, `build_file_ops_footer`.
- `coding-agent-branch-summary-test`: loads `test/fixtures/phase3_branch_session.jsonl` and asserts `BranchSummaryData` lists and summary footer.

### Manual scenario (live provider)

Session that reads and edits several files, then compacts. Check JSONL:

- compaction row has `read_files` and `modified_files` arrays
- branch_summary row has `read_files` and `modified_files` arrays
- summary text ends with `## Files Read` / `## Files Modified` sections

```bash
cat ~/.config/coding-agent/sessions/<latest>.jsonl | python3 -c "
import sys, json
for line in sys.stdin:
    row = json.loads(line.strip())
    if row.get('type') in ('compaction', 'branch_summary'):
        print(json.dumps(row, indent=2))
"
```

Validation status:

- [x] Build passes (`cmake --build ports/coding-agent/build -j`)
- [x] Local deterministic tests (`ctest --test-dir ports/coding-agent/build`)
- [ ] Manual scenario run and JSONL inspection with live provider connectivity
  - Optional when endpoint is available; local tests cover non-provider logic.

## Acceptance Criteria

- [x] `read_files` and `modified_files` correctly reflect tool calls (verified by `coding-agent-fileops-test` and branch-summary fixture test).
- [ ] Successive compactions carry forward file ops from prior windows (requires provider-backed compaction run or future session-store unit test with injectable session path).
- [x] Branch summary includes file ops extracted from the session file (fixture + `coding-agent-branch-summary-test`).
- [x] Summary text includes a file footer when file ops are non-empty (asserted in branch-summary test).
- [x] Build passes with zero errors and zero new warnings.

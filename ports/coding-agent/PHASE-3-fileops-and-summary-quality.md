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

- [ ] Implement `extract_file_ops_from_messages(messages)` in a new shared header/source (`file_ops.hpp`, `file_ops.cpp`):
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
- [ ] Helper: `merge_file_ops(FileOps& dst, const FileOps& src)` to accumulate across compaction windows.

**Files:** `file_ops.hpp` (new), `file_ops.cpp` (new)

---

### 2. Carry forward file ops from prior compaction rows

**Must-have**

- [ ] When loading session from JSONL, if a `compaction` row has `read_files`/`modified_files` arrays, hydrate a `FileOps` object and store it on the session store.
- [ ] In `compact_history()`, before extraction: seed `FileOps` from the prior compaction's persisted details so cumulative tracking is preserved across multiple compactions.

**Files:** `session.hpp`, `session.cpp`, `compaction.hpp`, `compaction.cpp`

---

### 3. Persist file ops in compaction JSONL rows

**Must-have**

- [ ] Extend `CompactionEvent`:
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
- [ ] `append_compaction()`: serialize `read_files` and `modified_files` arrays into JSONL row.
- [ ] `CompactionStats`: add `FileOps file_ops` field populated before calling the provider.

**Files:** `session.hpp`, `session.cpp`, `compaction.hpp`, `compaction.cpp`

---

### 4. Append file-op footer to compaction summaries

**Must-have**

- [ ] After generating the summary text in `compact_history()`, if `read_files` or `modified_files` are non-empty, append a footer block:
  ```
  ## Files Read
  - path/to/file.cpp
  ...

  ## Files Modified
  - path/to/other.cpp
  ...
  ```
- [ ] Dedup and sort both lists before appending.

**Files:** `compaction.cpp`, `file_ops.hpp`

---

### 5. File ops in branch summary

**Must-have**

- [ ] Update `BranchSummaryEvent`:
  ```cpp
  struct BranchSummaryEvent {
    std::string summary;
    std::string source_session_id;
    std::vector<std::string> read_files;
    std::vector<std::string> modified_files;
  };
  ```
- [ ] In `summarize_branch_session_file()`: call `extract_file_ops_from_messages()` on the session content, append the file footer to the summary, and return `read_files`/`modified_files` alongside.
- [ ] Persist file-op arrays in `branch_summary` JSONL row.

**Files:** `branch_summary.hpp`, `branch_summary.cpp`, `session.hpp`, `session.cpp`

---

### 6. Wire into `CMakeLists.txt`

**Must-have**

- [ ] The `GLOB_RECURSE` in `CMakeLists.txt` already picks up new `.cpp` files in `src/`, so no manual changes needed — verify new files are compiled.

**Files:** `CMakeLists.txt` (verify only)

---

## Validation

```bash
# Build
cmake -S ports/coding-agent -B ports/coding-agent/build
cmake --build ports/coding-agent/build -j

# Manual scenario: session that reads and edits several files, then compacts
# Check JSONL:
# - compaction row has non-empty read_files and modified_files arrays
# - branch_summary row has non-empty read_files and modified_files arrays
# - summary text ends with ## Files Read / ## Files Modified sections
cat ~/.config/coding-agent/sessions/<latest>.jsonl | python3 -c "
import sys, json
for line in sys.stdin:
    row = json.loads(line.strip())
    if row.get('type') in ('compaction', 'branch_summary'):
        print(json.dumps(row, indent=2))
"
```

## Acceptance Criteria

- [ ] `read_files` and `modified_files` correctly reflect tool calls made in the compacted window.
- [ ] Successive compactions carry forward file ops from prior windows.
- [ ] Branch summary includes file ops extracted from the session file.
- [ ] Summary text includes a file footer when file ops are non-empty.
- [ ] Build passes with zero errors and zero new warnings.

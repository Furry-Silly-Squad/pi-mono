# Phase 4: Branch Summary Model Parity

## Goal
Replace the session-snippet heuristic with true branch-path summarization semantics, matching the TypeScript reference.

## Complexity: High

## Prerequisites
- Phase 1 complete: stable entry IDs.
- Phase 3 complete: file-op extraction utility.

---

## Tasks

### 1. Add parent ID to session entry rows

**Must-have**

- [ ] Add `parent_id` field to persisted JSONL rows (all types: `message`, `compaction`, `branch_summary`).
- [ ] On `append()`: set `parent_id` to the ID of the last written row (maintaining a linked list in the session file).
- [ ] On load: populate an in-memory `id -> parent_id` map for branch traversal.
- [ ] Legacy rows without `parent_id` are loaded with an empty parent — they form a contiguous linear chain; treat them as a single branch.

**Files:** `session.hpp`, `session.cpp`

---

### 2. Branch traversal primitives

**Must-have**

- [ ] Implement `get_branch(entry_id)` on `SessionStore`:
  - Walks `parent_id` links from the given entry back to the root.
  - Returns ordered list of entry IDs from root to leaf.
- [ ] Implement `find_common_ancestor(id_a, id_b)`:
  - Build ancestor set from one path, walk the other, return deepest common node ID.
  - Returns empty string if no common ancestor.

**Files:** `session.hpp`, `session.cpp`

---

### 3. Collect entries for branch summarization

**Must-have**

- [ ] Implement `collect_entries_for_branch_summary(old_leaf_id, target_id)`:
  - Calls `find_common_ancestor(old_leaf_id, target_id)`.
  - Walks from `old_leaf_id` back to the common ancestor, collecting all entries.
  - Returns entries in chronological order (reversed from the walk).
  - Returns empty if no old leaf is set.

**Files:** `branch_summary.hpp`, `branch_summary.cpp`

---

### 4. Token-budget-aware branch entry preparation

**Must-have**

- [ ] Implement `prepare_branch_entries(entries, token_budget)`:
  - Walk entries newest to oldest, accumulate estimated token counts.
  - Stop when budget is exceeded.
  - Compaction and branch-summary entries get priority: if budget is within 10% of limit, still try to include them.
  - Extract file ops from all entries (regardless of budget); this ensures cumulative file tracking.
  - Return `{messages, file_ops, total_tokens}`.

**Files:** `branch_summary.hpp`, `branch_summary.cpp`

---

### 5. LLM-based branch summarization

**Must-have**

- [ ] Implement `generate_branch_summary(entries, provider, model, config)`:
  - Calls `prepare_branch_entries()` to get messages/file-ops within token budget.
  - Serializes conversation to plain text (role: content format) before passing to LLM.
  - Uses the branch-summary prompt structure:
    ```
    ## Goal
    ## Constraints & Preferences
    ## Progress
    ### Done / In Progress / Blocked
    ## Key Decisions
    ## Next Steps
    ```
  - Prepends "The user explored a different conversation branch before returning here." preamble.
  - Appends file-op footer (from Phase 3 utility).
  - Returns `BranchSummaryResult { summary, read_files, modified_files }`.
- [ ] On LLM failure: return a fallback summary with the snippet approach from the current implementation.

**Files:** `branch_summary.hpp`, `branch_summary.cpp`

---

### 6. Session switch integration in `agent.cpp`

**Must-have**

- [ ] When `--new-session` is used: record current session leaf ID before creating new session.
- [ ] After new session is initialized: call `collect_entries_for_branch_summary(old_leaf, "")` and then `generate_branch_summary()`.
- [ ] Persist resulting `BranchSummaryEvent` in the new session (already implemented in Phase 1).
- [ ] When `--session <id>` resumes a specific session: similarly generate branch summary from current to target if a branch divergence exists.

**Nice-to-have**

- [ ] Add `--no-branch-summary` flag to disable branch summarization for `--new-session`.

**Files:** `agent.cpp`, `config.hpp`, `config.cpp`

---

### 7. Context injection parity

**Must-have**

- [ ] When loading messages for a resumed session, only inject the branch-summary context message if the `source_session_id` is not on the current branch path.
- [ ] Skip injection if the branch summary was generated from the same branch (prevents double-injection on naive reload).

**Files:** `session.cpp`

---

## Validation

```bash
# Build
cmake -S ports/coding-agent -B ports/coding-agent/build
cmake --build ports/coding-agent/build -j

# Manual scenario 1: run a session, do some file edits, then --new-session
ports/coding-agent/build/coding-agent --base-url http://... --prompt "read README.md"
ports/coding-agent/build/coding-agent --base-url http://... --new-session --prompt "what did the previous session do?"

# Check: branch_summary row in new session JSONL contains relevant prior work
# Check: summary is LLM-generated (prose), not just raw line snippets
cat ~/.config/coding-agent/sessions/<new-session>.jsonl | python3 -c "
import sys, json
for line in sys.stdin:
    row = json.loads(line.strip())
    if row.get('type') == 'branch_summary':
        print(json.dumps(row, indent=2))
"

# Manual scenario 2: resume a different session with --session <old-id>
# Check: branch delta is computed correctly relative to the common ancestor
```

## Acceptance Criteria

- [ ] All session rows have `id` and `parent_id` fields.
- [ ] `get_branch()` returns correct path from root to any leaf.
- [ ] `find_common_ancestor()` returns correct result for linear and branched sessions.
- [ ] Branch summary is generated by LLM (structured prose), not raw line snippets.
- [ ] Branch summary includes file-op footer from actual tool calls.
- [ ] Fallback to snippet method when LLM call fails.
- [ ] Branch summary context is injected on `--new-session` and `--session <id>` only when relevant.
- [ ] Build passes with zero errors and zero new warnings.

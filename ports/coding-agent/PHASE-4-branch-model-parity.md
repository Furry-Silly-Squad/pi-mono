# Phase 4: Branch Summary Model Parity

## Goal
Replace the session-snippet heuristic with true branch-path summarization semantics, matching the TypeScript reference.

## Complexity: High

## Prerequisites
- Phase 1 complete: stable entry IDs.
- Phase 3 complete: file-op extraction utility.

---

## Status (current codebase)

Tasks **1–6** and **7** are implemented. Branch traversal and summarization live in `session.cpp` / `session.hpp`, `branch_summary.cpp` / `branch_summary.hpp`, and `agent.cpp`. Automated coverage: `coding-agent-branch-traversal-test` (graph load, `get_branch`, `find_common_ancestor`, `collect_entries_for_branch_summary`, `prepare_branch_entries`).

**Intentional gaps vs the original checklist wording**

- **`compaction_skipped` rows** (written ad hoc in `agent_loop.cpp`) do not carry `id` / `parent_id` in JSON; `load_session_graph()` still assigns synthetic ids and chains them using file-order `prev_row_id`, so they remain a linear continuation of the file. First-class append APIs (`append`, `append_compaction`, `append_branch_summary`) write `id` and `parent_id` explicitly.
- **Cross-session `--session` handoff** summarizes the **latest-on-disk** session file from its leaf toward the root (`collect_entries_for_branch_summary(old_graph, leaf, "")`). It does **not** compute a common ancestor between two session *files* or two arbitrary entry IDs across files; intra-file fork divergence uses `find_common_ancestor` only when `target_id` is non-empty in `collect_entries_for_branch_summary`.

---

## Tasks

### 1. Add parent ID to session entry rows

**Must-have**

- [x] Add `parent_id` field to persisted JSONL rows for types written via `SessionStore`: `session` (header), `message`, `compaction`, `branch_summary`.
- [x] On `append()` / `append_compaction()` / `append_branch_summary()`: set `parent_id` to the id of the last written row (`last_written_entry_id_`).
- [x] On load: `load_session_graph()` builds `SessionGraph::nodes` with `parent_id` on each `SessionNode`. Rows without `parent_id` use the previous row’s id (linear chain). Rows without `id` get a synthetic `legacy_<line>` id.
- [x] Legacy rows without `parent_id` form a contiguous linear chain; treated as a single branch.

**Files:** `session.hpp`, `session.cpp`

---

### 2. Branch traversal primitives

**Must-have**

- [x] `SessionGraph::get_branch(entry_id)` walks `parent_id` links from the given entry toward the root (cycle guard), returns ordered ids **root → leaf**.
- [x] `SessionGraph::find_common_ancestor(id_a, id_b)` builds ancestor set from one path, walks the other from leaf toward root, returns deepest common node id; empty string if none (including when either id is empty).
- [x] `SessionStore::get_branch` / `find_common_ancestor` delegate to `graph()`.

**Files:** `session.hpp`, `session.cpp`

---

### 3. Collect entries for branch summarization

**Must-have**

- [x] `collect_entries_for_branch_summary(SessionGraph const& graph, old_leaf_id, target_id)`:
  - If `target_id` is empty: walk from `old_leaf_id` up to (but not including) the `session` header row; reverse to chronological order.
  - If `target_id` is non-empty: `find_common_ancestor(old_leaf_id, target_id)`, then walk from `old_leaf_id` up toward but **not including** the ancestor, collecting nodes; reverse to chronological order.
  - Returns empty if `old_leaf_id` is empty.

**Files:** `branch_summary.hpp`, `branch_summary.cpp`

---

### 4. Token-budget-aware branch entry preparation

**Must-have**

- [x] `prepare_branch_entries(entries, token_budget)`:
  - Merges file ops from **all** entries first (`merge_row_file_ops` + per-message `extract_file_ops_from_messages` for picked messages).
  - Walks entries **newest to oldest**, estimates tokens (`usage_tokens` for assistant when set, else `approx_tokens` on content/tool args).
  - Stops when budget would be exceeded; **compaction** and **branch_summary** rows may still be included if over budget but total is still **below 90%** of `token_budget` (partial priority bump).
  - Returns `PreparedBranchEntries { messages, file_ops, total_tokens }`.

**Files:** `branch_summary.hpp`, `branch_summary.cpp`

---

### 5. LLM-based branch summarization

**Must-have**

- [x] `generate_branch_summary(entries, provider, model, context_size, reserve_tokens, error)`:
  - Uses `prepare_branch_entries(entries, max(0, context_size - reserve_tokens))`.
  - Serializes picked messages to plain text (`[User]:` / `[Assistant]:` / tool-call lines) inside `<conversation>...</conversation>` plus the structured branch-summary instructions.
  - Branch-format prompt matches the Goal / Constraints / Progress / Key Decisions / Next Steps structure (see `kBranchSummaryPrompt` in `branch_summary.cpp`).
  - Prepends preamble `kBranchPreamble` (`The user explored...` plus `Summary of that exploration:`).
  - Appends file-op footer via `build_file_ops_footer` on merged `FileOps`.
  - Returns `BranchSummaryResult { summary, read_files, modified_files }`.
- [x] On empty prepared messages or LLM failure (`provider.chat` false): `fallback_snippet_from_entries()` — snippet-style summary + same footer semantics.

**Files:** `branch_summary.hpp`, `branch_summary.cpp`

---

### 6. Session switch integration in `agent.cpp`

**Must-have**

- [x] When `--new-session`: before `start_or_resume`, resolve **latest** `.jsonl` in the session directory and record its path + `SessionGraph::leaf_id` if the graph loads and `leaf_id` is non-empty.
- [x] After the **new** session is created via `start_or_resume`, if a handoff was recorded: load that file as `old_graph`, `collect_entries_for_branch_summary(old_graph, old_leaf, "")`, then `generate_branch_summary(...)`, then `append_branch_summary` with `source_session_id` = old file stem and `handoff_source_leaf_id` = old leaf id.
- [x] When **`--session <id>`** and `<id>` is **not** the latest session file on disk: same handoff from the **latest** file’s leaf into the session being opened (summarize the abandoned “latest” session, not a divergent branch inside the target file).
- [x] If `branch_summary` is skipped or no valid prior graph/leaf, no row is appended.

**Nice-to-have**

- [x] `--no-branch-summary` sets `config.branch_summary = false` and skips the above.

**Files:** `agent.cpp`, `config.hpp`, `config.cpp`

---

### 7. Context injection parity

**Must-have**

**Resolved semantics (implementation):** Session **file** identity (`source_session_id`, the JSONL stem) is **not** comparable to **entry IDs** on the branch path. Injection control therefore uses the optional persisted field **`handoff_source_leaf_id`**: the **source session entry ID** of the summarized leaf when the handoff row was written.

- [x] Build the set of entry IDs on the **current leaf path** (root → current `SessionGraph::leaf_id` via `get_branch(leaf_id)`).
- [x] For each `branch_summary` row when reloading linear history: **omit** the synthetic assistant “branch handoff” message if **`handoff_source_leaf_id` is non-empty and appears on that path**.
- [x] If **`handoff_source_leaf_id` is empty** or **not on the path**, **inject** the branch-summary line (e.g. cross-session handoff where old-session entry IDs do not appear in the new file’s graph).

**Files:** `session.cpp` (`SessionStore::load_messages`)

---

## Validation

```bash
# Build
cmake -S ports/coding-agent -B ports/coding-agent/build
cmake --build ports/coding-agent/build -j

# Automated traversal / collection / prepare smoke test
./ports/coding-agent/build/coding-agent-branch-traversal-test

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

# Manual scenario 2: resume an older session while a newer file exists (--session <older-id>)
# Check: branch_summary is appended summarizing the latest session file you are leaving (see task 6)
```

## Acceptance Criteria

- [x] Session rows written through `SessionStore` APIs have `id` and `parent_id` where applicable; legacy and minimal rows load with synthetic ids / chained parents.
- [x] `get_branch()` returns correct path from root to any leaf (see `branch_traversal_test.cpp`).
- [x] `find_common_ancestor()` returns correct result for forked paths in one file (see test).
- [x] Branch summary is generated by LLM when the provider call succeeds (structured prose); otherwise snippet fallback.
- [x] Branch summary includes file-op footer from merged tool/compaction/branch file ops.
- [x] Fallback to snippet method when LLM call fails or no messages survive token budgeting.
- [x] Branch summary context is injected on reload **except** when skipped per task 7 (`handoff_source_leaf_id` on the current leaf path).
- [ ] Build passes with zero errors and zero new warnings (verify locally after changes).

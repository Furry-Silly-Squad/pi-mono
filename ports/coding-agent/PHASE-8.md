# Phase 8: SessionManager Tree Model

## Goal

Replace `SessionStore` with `SessionManager` — a full tree-model session class that supports all entry types (messages, compaction, branch summary, labels, custom entries, session info, model/thinking-level changes), tree traversal with `getTree()` / `getBranch()`, branching, labels, and version migration. This is the structural backbone that the TS original's `SessionManager` provides and the C++ port is missing.

## Complexity: Medium-High

## Prerequisites

- Phase 7 complete: `AgentSession` implemented, `agent_loop` deleted.
- Build passes with zero errors and zero new warnings before starting.

---

## Current State of the Port (as of Phase 8 completion)

### What exists

| Area | Status |
|------|--------|
| `SessionManager` | Full tree-model session class with all 10 entry types, tree traversal, branching, labels, migration |
| `SessionEntry` | `std::variant` of message, compaction, branch_summary, label, custom, custom_message, session_info, thinking_level_change, model_change, compaction_skipped |
| `SessionTreeNode` | Rooted tree with children, labels, sorted by timestamp |
| `SessionInfo` | Session metadata (path, id, cwd, name, timestamps, message count) |
| `SessionContext` | Messages + thinkingLevel + model, built via `buildSessionContext()` |
| `branch_summary.cpp` | Fully migrated to `SessionEntry` types |
| `AgentSession` | Uses `SessionManager&` for all persistence |

### What remains (minor cleanup)

| Area | Gap |
|------|-----|
| **Old files** | ~~`session.cpp`/`session.hpp` still on disk but not compiled (orphaned, ~597 lines)~~ ✅ Deleted in commit 4594de1c |
| **Interactive commands** | `/new` and `/branch` not yet wired in `interactive_mode.cpp` |
| **Test coverage** | Session-store/branch-traversal tests rewritten; label, migration, and branching tests may need expansion |

---

## Phase 8 Scope

### 1. New types and enums

- [x] `SessionEntry` — `std::variant` of all 9 entry types.
- [x] `SessionHeader` — `type`, `version`, `id`, `timestamp`, `cwd`, `parentSession`.
- [x] `SessionMessageEntry` — `type`, `id`, `parentId`, `timestamp`, `message` (ChatMessage).
- [x] `CompactionEntry` — `type`, `id`, `parentId`, `timestamp`, `summary`, `firstKeptEntryId`, `tokensBefore`, optional `details`.
- [x] `BranchSummaryEntry` — `type`, `id`, `parentId`, `timestamp`, `fromId`, `summary`, optional `details`.
- [x] `LabelEntry` — `type`, `id`, `parentId`, `timestamp`, `targetId`, `label`.
- [x] `CustomEntry` — `type`, `id`, `parentId`, `timestamp`, `customType`, optional `data` (nlohmann::json).
- [x] `CustomMessageEntry` — `type`, `id`, `parentId`, `timestamp`, `customType`, `content`, `display`, optional `details`.
- [x] `SessionInfoEntry` — `type`, `id`, `parentId`, `timestamp`, optional `name`.
- [x] `ThinkingLevelChangeEntry` — `type`, `id`, `parentId`, `timestamp`, `thinkingLevel`.
- [x] `ModelChangeEntry` — `type`, `id`, `parentId`, `timestamp`, `provider`, `modelId`.
- [x] `SessionTreeNode` — `entry`, `children`, optional `label`, optional `labelTimestamp`.
- [x] `SessionInfo` — `path`, `id`, `cwd`, `name`, `parentSessionPath`, `created`, `modified`, `messageCount`, `firstMessage`, `allMessagesText`.
- [x] `SessionContext` — `messages`, `thinkingLevel`, `model`.
- [x] `SessionListProgress` — callback type.
- [x] `FileEntry` — flattened `std::variant` of header + all entry types.

### 2. Entry ID generation

- [x] `generateId(std::unordered_set<std::string>& used_ids)` — 8-hex-char collision-checked IDs, fallback to full UUID.
- [x] `nowTimestamp()` — ISO 8601 UTC timestamp.
- [x] `sessionIdPrefix()` — millisecond timestamp prefix for session IDs.
- [x] Per-session `byId` index replaces global `entry_counter`.

### 3. `SessionManager` class (header + implementation)

Implementation: `session_entry.hpp` / `session_entry.cpp`. Helpers: `agent_sessions_root_directory()`, `session_directory_for_cwd(cwd)`, `SessionManager::openBySessionId(cwd, sessionId, sessionDir?)`.

**Construction / factories**

- [x] Private constructor: `(cwd, sessionDir, sessionFile, persist)`.
- [x] `static create(cwd, sessionDir?)` — new session with default session dir.
- [x] `static open(path, sessionDir?, cwdOverride?)` — load existing session file.
- [x] `static continueRecent(cwd, sessionDir?)` — find most recent `.jsonl` or create new.
- [x] `static inMemory(cwd)` — no file persistence.
- [x] `static forkFrom(sourcePath, targetCwd, sessionDir?)` — copy all entries from source into new session.

**Session lifecycle**

- [x] `newSession(options?)` — create header, clear `fileEntries`/`byId`/`labelsById`, optionally set `sessionFile`.
- [x] `setSessionFile(path)` — load existing file, migrate if needed, build index.
- [x] `isPersisted()` — bool.

**Entry append methods** (each appends as child of current `leafId`, advances leaf, returns entry id)

- [x] `appendMessage(message)` → `SessionMessageEntry`.
- [x] `appendCompaction(summary, firstKeptEntryId, tokensBefore, details?)` → `CompactionEntry`.
- [x] `appendBranchSummary(fromId, summary, details?)` → `BranchSummaryEntry`.
- [x] `appendLabelChange(targetId, label?)` → `LabelEntry` (updates `labelsById`).
- [x] `appendCustomEntry(customType, data?)` → `CustomEntry`.
- [x] `appendCustomMessageEntry(customType, content, display, details?)` → `CustomMessageEntry`.
- [x] `appendSessionInfo(name)` → `SessionInfoEntry`.
- [x] `appendThinkingLevelChange(level)` → `ThinkingLevelChangeEntry`.
- [x] `appendModelChange(provider, modelId)` → `ModelChangeEntry`.

**Tree traversal**

- [x] `getLeafId()` / `getLeafEntry()` — current leaf access.
- [x] `getEntry(id)` — fast lookup via `byId`.
- [x] `getChildren(parentId)` — all direct children of an entry.
- [x] `getBranch(fromId?)` — walk from entry to root, return entries in chronological order.
- [x] `getTree()` — build `SessionTreeNode` tree, resolve labels, sort children by timestamp.
- [x] `getLabel(id)` / `getEntries()` / `getHeader()`.

**Label management**

- [x] `labelsById` map (id → label) + `labelTimestampsById` map.
- [x] `appendLabelChange()` updates both maps and persists.
- [x] `getLabel()` reads from map.
- [x] Labels are preserved in `createBranchedSession()`.

**Branching**

- [x] `branch(branchFromId)` — set `leafId` to target entry.
- [x] `resetLeaf()` — set `leafId` to `nullptr` (before any entries).
- [x] `branchWithSummary(branchFromId?, summary, details?)` — same as `branch()` + append `BranchSummaryEntry`.
- [x] `createBranchedSession(leafId)` — extract path to leaf into a new session file, preserving labels.

**Persistence**

- [x] `fileEntries` vector (all entries including header).
- [x] `byId` map for fast lookup.
- [x] `_persist(entry)` — append-only or flush-on-first-assistant logic.
- [x] `_rewriteFile()` — write all `fileEntries` to session file.
- [x] `flushed` flag for deferred write until first assistant message.

**Migration**

- [x] `CURRENT_SESSION_VERSION = 3`.
- [x] `migrateToCurrentVersion(fileEntries)` — v1→v2 (add id/parentId), v2→v3 (rename hookMessage role).
- [x] Auto-run on `setSessionFile()` if loaded file has older version.

**Session info**

- [x] `getSessionName()` — walk entries in reverse to find latest `session_info` entry.
- [x] `buildSessionInfo(filePath)` — compute `SessionInfo` (messageCount, firstMessage, timestamps).
- [x] `list(cwd, sessionDir?, onProgress?)` — list all sessions for a directory.
- [x] `listAll(onProgress?)` — list all sessions across all project directories.

### 4. `buildSessionContext()` equivalent

- [x] Port `buildSessionContext(entries, leafId?, byId?)` from TS: walks from leaf to root, collects messages, handles compaction (summary + kept messages + post-compaction messages), handles branch summaries and custom messages.

### 5. Wire `SessionManager` into `AgentSession`

- [x] `AgentSession` constructor takes `SessionManager&` instead of `SessionStore&`.
- [x] `AgentSession` uses `sessionManager.appendMessage()` for user/assistant/tool messages.
- [x] `AgentSession` uses `sessionManager.appendThinkingLevelChange()` / `appendModelChange()` for state changes.
- [x] `AgentSession` uses `sessionManager.appendCompaction()` for compaction events.
- [x] `AgentSession::session_id()` delegates to `sessionManager.getSessionId()`.
- [x] `AgentSession::session_path()` delegates to `sessionManager.getSessionFile()`.
- [x] In-memory `messages_` is initialized from `sessionManager.buildSessionContext()` and updated each turn (same observable behavior as loading via `SessionStore`).

### 6. Update `main.cpp` and mode functions

- [x] `agent.cpp` creates `SessionManager` (`create` / `continueRecent` / `openBySessionId`) instead of `SessionStore`.
- [x] `interactive_mode.cpp` and `print_mode.cpp` signatures unchanged (still take `AgentSession&`).
- [ ] `/new` command in interactive mode: create new session via `SessionManager::create()` (CLI `--new-session` uses `SessionManager`; TUI `/new` not yet wired).
- [ ] `/branch` command (if exists): use `sessionManager.branch()`.

### 7. Remove `SessionStore` and `SessionGraph`

- [x] `session.cpp` / `session.hpp` removed from CMakeLists.txt (no longer compiled).
- [ ] Delete `session.hpp` / `session.cpp` files entirely (orphaned but still on disk).
- [ ] Update all includes and forward declarations.

### 8. Update `branch_summary.cpp`

- [x] `collect_entries_for_branch_summary()` migrated to use `SessionEntry` types.
- [x] `prepare_branch_entries()` works with `SessionEntry` types.

### 9. Build clean with `-Werror`

- [x] Zero errors, zero new warnings (port builds with `-Werror`).
- [x] All 6 tests pass (fileops, branch-summary, session-store, branch-traversal, compaction-carry-forward, agent-session).

---

## Tasks

### 1. New types (`session_manager.hpp`)

Create `session_manager.hpp` with all new entry types, `SessionTreeNode`, `SessionInfo`, and the `SessionManager` class declaration. Use a struct-based tagged-union approach (matching the TS `SessionEntry` union) with a `type` string discriminator and `std::optional` fields for type-specific data.

### 2. Entry ID generation (`session_manager.cpp`)

Implement `generateId()` with collision checking. Replace the global `entry_counter` with a per-session `byId` index.

### 3. `SessionManager` core (`session_manager.cpp`)

Implement constructor, factories, `newSession()`, `setSessionFile()`, `_buildIndex()`, `_persist()`, `_rewriteFile()`.

### 4. Entry append methods (`session_manager.cpp`)

Implement all `appendXXX()` methods. Each creates the appropriate typed entry, appends to `fileEntries`, updates `byId`/`labelsById`, persists, and returns the entry id.

### 5. Tree traversal (`session_manager.cpp`)

Implement `getLeafId()`, `getLeafEntry()`, `getEntry()`, `getChildren()`, `getBranch()`, `getTree()`, `getLabel()`, `getEntries()`, `getHeader()`.

### 6. Label management (`session_manager.cpp`)

Implement `labelsById` + `labelTimestampsById` maps, `appendLabelChange()`, `getLabel()`.

### 7. Branching (`session_manager.cpp`)

Implement `branch()`, `resetLeaf()`, `branchWithSummary()`, `createBranchedSession()`.

### 8. Migration (`session_manager.cpp`)

Implement `migrateToCurrentVersion()`, `migrateV1ToV2()`, `migrateV2ToV3()`. Run automatically on file load.

### 9. Session info + listing (`session_manager.cpp`)

Implement `getSessionName()`, `buildSessionInfo()`, `list()`, `listAll()`.

### 10. `buildSessionContext()` (`session_manager.cpp`)

Port the TS `buildSessionContext()` function. Walks from leaf to root, handles compaction/branch-summary resolution, returns `SessionContext` (messages + thinkingLevel + model).

### 11. Wire into `AgentSession`

Update `AgentSession` to use `SessionManager` instead of `SessionStore`. Update all message append paths.

**Done** — `AgentSession` takes `SessionManager&`; persistence and compaction wired.

### 12. Update `main.cpp` + mode functions

Update `agent.cpp` to create `SessionManager`. Ensure interactive/print modes work unchanged.

**Done** — `run_agent` constructs `SessionManager` and passes it to `AgentSession`; branch-summary handoff uses `appendBranchSummary`.

### 13. Update `branch_summary.cpp`

Adapt to use `SessionManager::getBranch()` instead of `SessionGraph`.

### 14. Remove `SessionStore` / `SessionGraph`

Delete old files. Update includes.

### 15. Build clean

`cmake --build ports/coding-agent/build -j` with `-Werror`.

### 16. Update tests

Rewrite `session_store_roundtrip_test.cpp` and `branch_traversal_test.cpp` to use `SessionManager`. Add new tests for:
- Label add/remove/lookup
- Tree build with branching
- `createBranchedSession()` correctness
- Migration v1→v2, v2→v3
- `buildSessionContext()` with compaction
- `appendMessage` + `branch` + `appendMessage` tree structure

---

## Validation

```bash
# Build
cmake -S ports/coding-agent -B ports/coding-agent/build
cmake --build ports/coding-agent/build -j

# Run tests
cd ports/coding-agent/build && ctest --output-on-failure

# Smoke test interactive
ports/coding-agent/build/coding-agent --base-url http://beugul-desktop:8080
# → TUI, /compact, /stats, /new, Ctrl+C all work

# Smoke test print mode
ports/coding-agent/build/coding-agent --base-url http://beugul-desktop:8080 --prompt "say hi"

# Verify session file format (should have proper id/parentId tree, timestamps)
cat ~/.config/coding-agent/sessions/*.jsonl | head -20
```

## Acceptance Criteria

- [ ] `SessionManager` replaces `SessionStore` entirely; `SessionStore`/`SessionGraph`/`SessionNode`/`SessionRowKind` deleted.
- [ ] All 10 entry types supported (message, compaction, branch_summary, label, custom, custom_message, session_info, thinking_level_change, model_change, compaction_skipped).
- [ ] `getTree()` returns a properly rooted tree with sorted children and resolved labels.
- [ ] `getBranch()` returns entries in chronological order from root to target.
- [ ] `getChildren()` returns all direct children of an entry.
- [ ] Labels can be added/removed/queried; persisted in session file.
- [ ] `branch()` moves leaf pointer; `branchWithSummary()` also appends summary.
- [ ] `createBranchedSession()` extracts a path into a new file with labels preserved.
- [ ] `buildSessionContext()` correctly resolves compaction (summary + kept messages + post-compaction) and branch summaries.
- [ ] Version migration v1→v2 and v2→v3 works automatically on file load.
- [ ] `SessionManager::create()`, `::open()`, `::continueRecent()`, `::inMemory()`, `::forkFrom()` all work.
- [x] `AgentSession` uses `SessionManager` for all message/state persistence.
- [ ] All existing interactive commands (`/compact`, `/stats`, `/tokens`, `/clear`, `/exit`, `/new`) work.
- [ ] Ctrl+C cancellation unchanged.
- [ ] TUI animation unchanged.
- [x] Build clean with `-Werror`.
- [x] All 6 tests pass (fileops, branch-summary, session-store, branch-traversal, compaction-carry-forward, agent-session).

---

## Migration Notes for Existing Session Files

Existing `.jsonl` files created by `SessionStore` have:
- No `timestamp` field on entries
- No `version` on session header
- `parent_id` assigned from previous row (not explicit in JSON)

`SessionManager` will:
1. Detect missing version (treat as v1)
2. Run migration: assign `id` (8-char hex), `parentId` from existing `parent_id`, add `timestamp`
3. Rewrite file with new format
4. Subsequent appends use the new format

This ensures backward compatibility with all existing session files.

---

## Missing Functionality Catalog (updated)

### Session Manager (session-manager.ts → session_manager.hpp/cpp)

| Feature | TypeScript | C++ Port (after Phase 8) |
|---------|-----------|--------------------------|
| SessionManager class | Full tree traversal, branching | **Phase 8: Full** |
| Session entry types (9+ types) | Yes | **Phase 8: Full** |
| Branch/leaf management | Yes | **Phase 8: Full** |
| Labels on entries | Yes | **Phase 8: Full** |
| Session migration | Yes | **Phase 8: Full** |
| Session context building | Yes | **Phase 8: Full** |
| Version tracking | Yes | **Phase 8: Full** |
| Session listing (local + all) | Yes | **Phase 8: Full** |
| Session forking | Yes | **Phase 8: Full** |
| In-memory sessions | Yes | **Phase 8: Full** |

---

## Deferred to Later Phases

These features from the TS `SessionManager` are intentionally deferred:

| Feature | Reason |
|---------|--------|
| `CustomMessageEntry` with image content | Image support deferred (no image pipeline in port) |
| `CustomEntry` with arbitrary JSON | Extension system deferred |
| `listAll()` with async I/O | C++ port uses sync I/O; can be simplified |
| Progress callbacks (`SessionListProgress`) | Not needed for CLI; can be added if TUI needs it |

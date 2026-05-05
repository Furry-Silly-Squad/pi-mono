# PHASE-14: Session Import from JSONL + Session Lifecycle Management

## Goal

Add session import from external JSONL files and a runtime-level session lifecycle management layer to the C++ port, matching the TypeScript `AgentSessionRuntime` pattern. This enables:

1. **Session import** — load an external JSONL session file (e.g., exported from the TS port, or from another project directory) and switch to it.
2. **Runtime-level session replacement** — a `SessionRuntime` class that owns the current `AgentSession` and manages teardown/rebind semantics for `/new`, `/switch`, `/fork`, and `/import` operations.
3. **Session lifecycle events** — emit `session_shutdown`, `session_before_switch`, `session_before_fork`, and `session_start` events during lifecycle transitions.

## TypeScript Reference

The TS implementation has two key pieces:

### `AgentSessionRuntime` (`packages/coding-agent/src/core/agent-session-runtime.ts`)

- Owns the current `AgentSession` and its cwd-bound `AgentSessionServices`.
- Provides `switchSession()`, `newSession()`, `fork()`, `importFromJsonl()`, `dispose()`.
- Each lifecycle method follows a teardown/rebind pattern:
  1. Emit `session_before_switch` (or `session_before_fork`) — extensions can cancel.
  2. Emit `session_shutdown` — tear down extensions, call `session.dispose()`.
  3. Create a new `AgentSession` via the `createRuntime` factory.
  4. Emit `session_start` with reason and previous session file.
  5. Call `rebindSession()` to re-register event handlers and extension bindings.
- Supports `withSession` callback to perform custom setup on the new session before returning.

### `SessionManager.importFromJsonl()` (`packages/coding-agent/src/core/session-manager.ts`)

- Copies the input JSONL file into the session directory.
- Opens it via `SessionManager.open()`.
- Follows the same teardown/rebind pattern as `switchSession()`.
- Supports `cwdOverride` to override the session's stored working directory.
- Throws `SessionImportFileNotFoundError` if the file doesn't exist.
- Throws `MissingSessionCwdError` if the session's cwd cannot be resolved and no override is provided.

### `/import` slash command (`packages/coding-agent/src/core/slash-commands.ts`)

- Prompts the user for a file path.
- Calls `runtime.importFromJsonl(path)`.
- Displays import result.

### `/switch`, `/fork` slash commands

- `/switch`: prompts for session path, calls `runtime.switchSession(path)`.
- `/fork`: prompts for entry ID, calls `runtime.fork(entryId)` with position ("before" or "at").
  - For "before": extracts the user message text from the target entry and uses it as the first message of the forked session.

## Current C++ State

The C++ port already has the building blocks:

- **`SessionManager`** (`session_entry.{hpp,cpp}`):
  - `create()` — create a new session.
  - `open(path)` — open an existing session file.
  - `continueRecent()` — continue or create.
  - `forkFrom(sourcePath, targetCwd)` — fork a session from another project directory.
  - `newSession(options)` — start a new session.
  - `setSessionFile(path)` — switch to a different session file.
  - `createBranchedSession(leafId)` — create a new session file from a branch path.
  - `switchSession()` — `AgentSession` method that swaps `session_` with a new `SessionManager`.
  - `createNewSession()` — `AgentSession` method that creates a new session and switches.
  - `branchFrom(id)` — `AgentSession` method that branches from a specific entry.

- **`AgentSession`**:
  - Has `switchSession(std::unique_ptr<SessionManager>)` for session replacement.
  - Has `createNewSession()` for new session creation.
  - Has `branchFrom()`, `branchWithSummary()` for branching.
  - Has event handler mechanism via `set_event_handler()`.
  - Has `dispose()` via destructor.

**What's missing:**

1. **No `importFromJsonl()`** — no way to load an external JSONL file into the session directory and switch to it.
2. **No runtime-level session management** — `AgentSession` methods for session replacement are scattered and don't follow a consistent teardown/rebind pattern.
3. **No lifecycle events** — no `session_shutdown`, `session_before_switch`, `session_before_fork`, or `session_start` events.
4. **No `/import` command** — no interactive import flow.
5. **No `/switch` command** — no interactive session switching.
6. **No `/fork` command with text extraction** — `branchFrom()` exists but no interactive fork flow with user message extraction.

## Design

### 1. Session Lifecycle Events

Add lifecycle event types to `AgentEvent::Type`:

```cpp
struct AgentEvent {
    enum class Type {
        // ... existing types ...
        SessionShutdown,      // Session is being torn down
        SessionBeforeSwitch,  // Before switching sessions (extensions can cancel)
        SessionBeforeFork,    // Before forking (extensions can cancel)
        SessionStart,         // New session is starting
    };
    // ...
    // For SessionShutdown
    std::string shutdown_reason;  // "new", "resume", "fork", "quit"
    std::string target_session_file;

    // For SessionBeforeSwitch / SessionBeforeFork
    std::string entry_id;          // For fork
    std::string position;          // "before" or "at" (for fork)
    bool cancelled = false;        // Extensions can set this

    // For SessionStart
    std::string start_reason;      // "new", "resume", "fork"
    std::string previous_session_file;
};
```

### 2. SessionRuntime — Runtime-Level Session Management

Create a `SessionRuntime` class that owns the current `AgentSession` and manages lifecycle transitions:

```cpp
// Forward declaration
class Provider;
class ToolRegistry;

/// Result of a session lifecycle operation.
struct SessionLifecycleResult {
    bool cancelled = false;
    std::string error;
};

/// Factory function for creating a new AgentSession.
using AgentSessionFactory = std::function<std::unique_ptr<AgentSession>(
    const AgentSessionConfig& config,
    Provider& provider,
    ToolRegistry& tools,
    std::unique_ptr<SessionManager> session
)>;

class SessionRuntime {
public:
    SessionRuntime(
        AgentSessionFactory factory,
        Provider& provider,
        ToolRegistry& tools,
        std::unique_ptr<SessionManager> initial_session,
        const AgentSessionConfig& config
    );

    ~SessionRuntime();

    /// Current session (read-only).
    AgentSession* session() const;

    /// Current session file path.
    std::string session_file() const;

    /// Working directory.
    std::string cwd() const;

    // ------------------------------------------------------------------
    // Lifecycle Operations
    // ------------------------------------------------------------------

    /// Switch to an existing session file.
    SessionLifecycleResult switchSession(
        const std::string& session_path,
        const std::optional<std::string>& cwd_override = std::nullopt
    );

    /// Create a new session.
    SessionLifecycleResult newSession(
        const std::optional<std::string>& parent_session = std::nullopt
    );

    /// Fork from a specific entry ID.
    SessionLifecycleResult fork(
        const std::string& entry_id,
        const std::string& position = "before"  // "before" or "at"
    );

    /// Import an external JSONL file.
    SessionLifecycleResult importFromJsonl(
        const std::string& input_path,
        const std::optional<std::string>& cwd_override = std::nullopt
    );

    /// Dispose the runtime and tear down the session.
    void dispose();

    // ------------------------------------------------------------------
    // Event Subscription
    // ------------------------------------------------------------------

    /// Set a callback for lifecycle events.
    void set_lifecycle_event_handler(std::function<void(const AgentEvent&)> handler);

private:
    // ------------------------------------------------------------------
    // Internal Helpers
    // ------------------------------------------------------------------

    /// Emit a lifecycle event and check for cancellation.
    SessionLifecycleResult emit_lifecycle_event(const AgentEvent& event);

    /// Teardown the current session (emit shutdown, dispose, clear).
    void teardown_session(const std::string& reason, const std::string& target_file);

    /// Create and apply a new session (factory + rebind).
    SessionLifecycleResult apply_new_session(
        std::unique_ptr<SessionManager> session_manager,
        const std::string& reason,
        const std::optional<std::string>& previous_file = std::nullopt
    );

    /// Rebind event handlers after session replacement.
    void rebind_session();

    // ------------------------------------------------------------------
    // State
    // ------------------------------------------------------------------

    AgentSessionFactory factory_;
    Provider& provider_;
    ToolRegistry& tools_;
    std::unique_ptr<AgentSession> session_;
    AgentSessionConfig config_;
    std::optional<std::string> previous_session_file_;

    std::function<void(const AgentEvent&)> lifecycle_event_handler_;
};
```

### 3. Session Import

Implement `importFromJsonl()` in `SessionRuntime`:

1. Resolve the input path (absolute).
2. Validate the file exists (throw `SessionImportFileNotFoundError` if not).
3. Copy the file into the current session directory (using `basename` for the filename).
4. If the copied path differs from the input, the file was copied; otherwise it was already in the session directory.
5. Emit `session_before_switch` with reason `"resume"` and target file path.
6. If not cancelled:
   - Open the session via `SessionManager::open()`.
   - Teardown the current session (`session_shutdown`).
   - Apply the new session via the factory.
   - Emit `session_start` with reason `"resume"` and previous session file.

### 4. Session Switch

Implement `switchSession()` in `SessionRuntime`:

1. Validate the session file exists.
2. Emit `session_before_switch` with reason `"resume"` and target file path.
3. If not cancelled:
   - Open the session via `SessionManager::open()`.
   - Teardown the current session.
   - Apply the new session.
   - Emit `session_start` with reason `"resume"`.

### 5. New Session

Implement `newSession()` in `SessionRuntime`:

1. Emit `session_before_switch` with reason `"new"`.
2. If not cancelled:
   - Create a new `SessionManager::create()` with optional `parentSession`.
   - Teardown the current session.
   - Apply the new session.
   - Emit `session_start` with reason `"new"`.

### 6. Fork

Implement `fork()` in `SessionRuntime`:

1. Emit `session_before_fork` with `entry_id` and `position`.
2. If not cancelled:
   - Look up the entry via `session_->sessionManager()->getEntry(entry_id)`.
   - If `position == "before"`:
     - Extract the user message text from the entry (for the first message of the forked session).
     - Get the parent entry ID (the leaf of the path to keep).
   - If `position == "at"`:
     - Use the entry ID directly as the leaf.
   - If the current session is persisted:
     - Create a branched session via `session_->sessionManager()->createBranchedSession(leaf_id)`.
     - Open the branched session.
     - Teardown and apply.
   - If not persisted:
     - Call `createBranchedSession()` on the current `SessionManager` (in-memory mode).
     - Teardown and apply.
   - Emit `session_start` with reason `"fork"`.

### 7. Interactive Commands

Add slash commands to `interactive_mode`:

- **`/import <path>`** — import a JSONL session file.
- **`/switch <path>`** — switch to an existing session file.
- **`/fork <entry_id>`** — fork from a specific entry (with optional `--position before|at`).

The commands use `SessionRuntime` for the actual lifecycle operations.

## File Layout

```
ports/coding-agent/src/
├── agent_session.hpp          # ADD: SessionShutdown, SessionBeforeSwitch, SessionBeforeFork, SessionStart event types
├── agent_session.cpp          # ADD: emit lifecycle events
├── session_entry.cpp          # ADD: importFromJsonl() helper (copy + open)
├── session_runtime.hpp        # NEW: SessionRuntime class
├── session_runtime.cpp        # NEW: SessionRuntime implementation
├── modes/
│   ├── interactive_mode.cpp   # ADD: /import, /switch, /fork commands
│   └── interactive_mode.hpp   # ADD: SessionRuntime* parameter
├── agent.cpp                  # MODIFY: create SessionRuntime instead of bare AgentSession
└── config.cpp                 # No changes (session lifecycle is runtime-level, not config)
```

## Session Import Details

### File Copy

```cpp
/// Copy a JSONL session file into the session directory.
/// Returns the destination path, or empty if the source is already in the target directory.
std::optional<std::string> copy_session_file(
    const std::string& source_path,
    const std::string& session_dir
);
```

- Use `std::filesystem::copy_file()` with `copy_options::overwrite_existing`.
- Destination: `join(session_dir, basename(source_path))`.
- If source and destination are the same file (same inode), return empty.

### Validation

```cpp
/// Validate that a JSONL file has a valid session header.
bool validate_session_file(const std::string& path);
```

- Read the first line.
- Parse as JSON.
- Check `type == "session"` and `id` is present.

### Error Handling

```cpp
/// Thrown when import references a non-existent file.
class SessionImportFileNotFoundError : public std::runtime_error {
public:
    explicit SessionImportFileNotFoundError(const std::string& file_path);
    std::string file_path() const;
};
```

## C++ vs TS Differences

| Aspect | TS | C++ (target) |
|--------|----|---------------|
| Session factory | `createAgentSessionFromServices()` with cwd-bound services | `AgentSessionFactory` callable; no services layer |
| Extension system | Full `ExtensionRunner` with hooks | No extension system (lifecycle events are emitted but no handlers) |
| `withSession` callback | Supported for custom session setup | Not supported (no extensions, no custom setup needed) |
| CWD resolution | `assertSessionCwdExists()` checks session header cwd | Session header `cwd` is read; if empty, falls back to `process.cwd()` equivalent |
| Session directory | Auto-created via `mkdirSync` | `std::filesystem::create_directories()` |
| File copy | `copyFileSync()` | `std::filesystem::copy_file()` |
| Error types | Custom error classes (`SessionImportFileNotFoundError`, `MissingSessionCwdError`) | `std::runtime_error` subclasses |

## Implementation Order

1. **Lifecycle event types** — add `SessionShutdown`, `SessionBeforeSwitch`, `SessionBeforeFork`, `SessionStart` to `AgentEvent`.
2. **`copy_session_file()` and `validate_session_file()`** — helper functions for import.
3. **`SessionRuntime` class** — the runtime-level session management layer.
4. **`importFromJsonl()`** — session import with file copy and validation.
5. **`switchSession()`** — session switching.
6. **`newSession()`** — new session creation.
7. **`fork()`** — fork with entry lookup and branch creation.
8. **`AgentSession` event emission** — emit lifecycle events during `switchSession()` and `createNewSession()`.
9. **`agent.cpp` integration** — create `SessionRuntime` instead of bare `AgentSession`.
10. **Interactive commands** — `/import`, `/switch`, `/fork` in `interactive_mode.cpp`.

## Testing

- Unit tests for `copy_session_file()` — same directory, different directory, same inode.
- Unit tests for `validate_session_file()` — valid header, invalid JSON, missing type, missing id.
- Unit tests for `SessionRuntime::importFromJsonl()` — valid file, non-existent file, cancelled by hook.
- Unit tests for `SessionRuntime::switchSession()` — valid session, non-existent file, cancelled.
- Unit tests for `SessionRuntime::fork()` — valid entry, invalid entry, "before" position, "at" position.
- Integration test: spawn `SessionRuntime`, import a session, verify messages match.
- Integration test: fork from an entry, verify branched session has correct path.
- Integration test: switch between sessions, verify event handlers are rebound.

## Dependencies

- No new dependencies. Uses existing nlohmann/json, stdlib.
- `std::filesystem` (C++17, already used in `session_entry.cpp`).

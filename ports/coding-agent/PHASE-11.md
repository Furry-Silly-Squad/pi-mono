# Phase 11: Sub-Agent Task Delegation

## Goal

Enable the main `coding-agent` process to decompose a user request into subtasks, spawn child `coding-agent` processes to execute them sequentially (GPU-bound), and aggregate results back into the parent session.

This is a **task delegation** model: one orchestrator (main process) → N workers (child processes). No parallelism (GPU-limited), no inter-agent communication beyond session files.

## Architecture

```
Main Process (coding-agent)
  ├── User: "Implement auth, add tests, update docs"
  ├── LLM call: decompose into subtasks
  ├── SubTaskEntry: track subtask state in parent session
  ├── Spawn child 1: "Implement auth module in src/auth/"
  │     └── child-1.jsonl (session file)
  ├── Collect result from child-1.jsonl
  ├── Spawn child 2: "Add tests for auth module"
  │     └── child-2.jsonl
  ├── Collect result
  ├── Spawn child 3: "Update docs in docs/auth.md"
  │     └── child-3.jsonl
  ├── Collect result
  └── Continue conversation with aggregated results
```

## Decomposition Format

### LLM Output Format

The main agent's first LLM call (triggered by a user message that looks like a multi-task request) produces **structured JSON** that the main agent parses. The JSON goes into a `subtask_decomposition` custom entry.

```json
{
  "decomposition": {
    "description": "User requested implementation of auth module, tests, and documentation",
    "subtasks": [
      {
        "id": "1",
        "description": "Implement auth module in src/auth/",
        "context_files": ["src/auth/auth.h", "src/auth/auth.cpp"],
        "expected_artifacts": ["src/auth/auth.h", "src/auth/auth.cpp"],
        "dependencies": [],
        "priority": 1
      },
      {
        "id": "2",
        "description": "Add unit tests for auth module",
        "context_files": ["src/auth/auth.h", "src/auth/auth.cpp"],
        "expected_artifacts": ["tests/auth.test.cpp"],
        "dependencies": ["1"],
        "priority": 2
      },
      {
        "id": "3",
        "description": "Update documentation in docs/auth.md",
        "context_files": ["docs/README.md"],
        "expected_artifacts": ["docs/auth.md"],
        "dependencies": ["1", "2"],
        "priority": 3
      }
    ]
  }
}
```

### Field Semantics

| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `id` | string | Yes | Unique subtask identifier (1, 2, 3, ...) |
| `description` | string | Yes | Natural language description of what to do |
| `context_files` | string[] | No | Files the sub-agent should read before starting |
| `expected_artifacts` | string[] | No | Files the sub-agent is expected to create/modify |
| `dependencies` | string[] | No | IDs of subtasks that must complete before this one |
| `priority` | int | No | Execution order hint (lower = higher priority) |

### System Prompt Addition

The main agent's system prompt gets a new section for Phase 11:

```
## Task Delegation (Phase 11)

If the user's request involves multiple distinct tasks (e.g., "implement X, add tests, update docs"),
you MUST decompose it into subtasks before executing. Respond with a JSON object matching this schema:

{
  "decomposition": {
    "description": "Brief summary of the user's request",
    "subtasks": [
      {
        "id": "1",
        "description": "Natural language description of what to do",
        "context_files": ["file1", "file2"],
        "expected_artifacts": ["output1", "output2"],
        "dependencies": [],
        "priority": 1
      }
    ]
  }
}

Rules:
- Each subtask should be self-contained and executable by a single coding-agent process
- Dependencies must form a DAG (no cycles)
- Priority is used for execution order (lower number = execute first)
- Context files are files the sub-agent should read before starting
- Expected artifacts are files the sub-agent is expected to create or modify
- If the request is a single task, respond with a single subtask
- If the request doesn't need decomposition, respond with a single subtask that handles the whole request
```

### Decomposition Decision Logic

The main agent decides whether to decompose based on heuristics:

1. **Multi-sentence requests** with distinct actions ("implement X, add tests, update docs")
2. **Explicit multi-task language** ("do A, then B, then C")
3. **Cross-module changes** that touch unrelated parts of the codebase
4. **User explicitly requests decomposition** (e.g., `/decompose` command)

If the request is a single coherent task, the main agent handles it directly without decomposition.

## Session Entry Type

### `SubTaskEntry`

A new session entry type to track subtask state in the parent session:

```cpp
struct SubTaskEntry : SessionEntryBase {
  std::string type = "subtask";
  std::string subtaskId;        // "1", "2", "3", ...
  std::string description;
  std::string state;            // "pending", "running", "completed", "failed", "skipped"
  std::string sessionId;        // Child session ID (e.g., "abc123")
  std::optional<std::string> resultSummary;  // First N chars of last assistant message
  std::vector<std::string> dependencies;
  std::optional<std::string> errorMessage;
};
```

### `SubTaskDecompositionEntry`

Stores the full decomposition JSON for reference:

```cpp
struct SubTaskDecompositionEntry : SessionEntryBase {
  std::string type = "subtask_decomposition";
  std::string description;
  nlohmann::json subtasks;  // Full JSON array from LLM
};
```

### Session Entry Variant Update

The `SessionEntry` variant gets two new types:

```cpp
using SessionEntry = std::variant<
    SessionMessageEntry,
    ThinkingLevelChangeEntry,
    ModelChangeEntry,
    CompactionEntry,
    BranchSummaryEntry,
    CustomEntry,
    LabelEntry,
    SessionInfoEntry,
    CustomMessageEntry,
    SubTaskEntry,           // NEW
    SubTaskDecompositionEntry  // NEW
>;
```

### JSONL Serialization

Both entry types serialize to JSONL with their respective fields. The `subtask_decomposition` entry stores the full LLM output for debugging and replay. The `subtask` entries track runtime state.

## Sub-Agent Spawn Logic

### Process Spawning

Each subtask spawns a child `coding-agent` process:

```cpp
// Pseudocode for subagent.hpp/cpp
struct SubAgentResult {
  std::string sessionId;
  std::string sessionPath;
  std::string lastAssistantMessage;
  bool success;
  std::optional<std::string> error;
};

class SubAgent {
 public:
  /// Spawn a child coding-agent process for a subtask.
  /// Blocks until the child completes.
  static SubAgentResult spawn(
      const std::string& binaryPath,
      const std::string& subtaskDescription,
      const std::vector<std::string>& contextFiles,
      const std::string& parentSessionPath,
      const Config& parentConfig
  );

  /// Check if a GPU lock is available (file-based semaphore).
  static bool tryAcquireGpuLock(const std::string& lockPath);

  /// Release the GPU lock.
  static void releaseGpuLock(const std::string& lockPath);
};
```

### Child Process Command Line

```bash
coding-agent \
  --provider llama-cpp \
  --base-url http://127.0.0.1:8080 \
  --model <model-id> \
  --api-key <api-key> \
  --cwd <cwd> \
  --new-session \
  --session subtask-<parent-id>-<subtask-id>.jsonl \
  --max-tokens <max-tokens> \
  --temperature <temperature> \
  --no-branch-summary \
  "Implement auth module in src/auth/"
```

### GPU Semaphore

A simple file-based lock for GPU limiting:

```cpp
// gpu_semaphore.hpp/cpp
class GpuSemaphore {
 public:
  /// Try to acquire the GPU lock. Returns true if acquired, false if busy.
  static bool tryAcquire(const std::string& lockPath, int timeoutMs = 60000);

  /// Release the GPU lock.
  static void release(const std::string& lockPath);

  /// Check if the lock is held (by another process).
  static bool isHeld(const std::string& lockPath);
};
```

**Implementation:** Write PID + timestamp to lock file. On acquire, check if PID is alive. If dead, stale lock. If alive, wait. On release, delete lock file.

### Child Session Organization

Child sessions are stored under the parent session's directory:

```
~/.config/coding-agent/sessions/
  └── sessions/
      └── --home-user-project--/
          ├── 2025-01-01T00:00:00_abc123.jsonl  (parent session)
          └── subtask-abc123-1_...jsonl  (child 1)
          ├── subtask-abc123-2_...jsonl  (child 2)
          └── subtask-abc123-3_...jsonl  (child 3)
```

Child session filenames follow the pattern: `subtask-<parent-session-id>-<subtask-id>_<timestamp>_<child-session-id>.jsonl`

## Result Collection

### Reading Sub-Agent Results

After the child process completes, the parent reads the last assistant message from the child session:

```cpp
std::string readLastAssistantMessage(const std::string& sessionPath) {
  // Load session entries
  // Walk from leaf to root
  // Find last assistant message
  // Return content (up to N chars)
}
```

### Injecting Results into Parent Session

Each subtask result gets appended to the parent session as a `CustomMessageEntry`:

```
[tool: subtask_1] completed
→ Result: "Auth module implemented with login, logout, and token validation..."

[tool: subtask_2] completed
→ Result: "Unit tests added for auth module: 12 tests, all passing..."

[tool: subtask_3] completed
→ Result: "Documentation updated with API reference, usage examples..."
```

The parent agent then continues its turn with these results as context.

### Result Summary Format

The result summary stored in `SubTaskEntry.resultSummary` is the first 500 characters of the child's last assistant message. This gives the parent agent enough context to decide next steps without re-reading the full child session.

## Implementation Plan

### Files to Create

| File | Purpose |
|------|---------|
| `src/subagent.hpp` | SubAgent class, spawn logic, GPU semaphore |
| `src/subagent.cpp` | Implementation of spawn, result collection |
| `src/gpu_semaphore.hpp` | GpuSemaphore class (file-based lock) |
| `src/gpu_semaphore.cpp` | Implementation of GPU semaphore |

### Files to Modify

| File | Changes |
|------|---------|
| `src/session_entry.hpp` | Add `SubTaskEntry`, `SubTaskDecompositionEntry` to variant |
| `src/session_entry.cpp` | Add JSON serialization/deserialization for new entry types |
| `src/agent_session.hpp` | Add `decomposeAndExecute()`, `waitForSubAgents()` |
| `src/agent_session.cpp` | Implement decomposition logic, child spawn, result collection |
| `src/system_prompt.hpp/cpp` | Add Phase 11 decomposition section to system prompt |
| `src/config.hpp` | Add `subagent_enabled`, `gpu_lock_path` config fields |

### Implementation Steps

#### Step 1: Session Entry Types ✅ (design)

Add `SubTaskEntry` and `SubTaskDecompositionEntry` to the session entry variant. Implement JSON serialization/deserialization.

#### Step 2: GPU Semaphore ✅ (design)

Implement file-based GPU lock. Check PID liveness. Support timeout.

#### Step 3: SubAgent Spawn ✅ (design)

Implement `SubAgent::spawn()` that:
- Acquires GPU lock (waits with timeout)
- Constructs child command line
- Spawns child via `popen()` or `fork()` + `exec()`
- Waits for child to complete
- Collects exit code and session file path
- Releases GPU lock

#### Step 4: Result Collection ✅ (design)

Implement result reading from child session files. Parse last assistant message.

#### Step 5: AgentSession Integration ✅ (design)

Add `decomposeAndExecute()` method to `AgentSession`:
- Detects multi-task requests via heuristics or `/decompose` command
- Calls LLM to decompose into subtasks
- Creates `SubTaskDecompositionEntry` in parent session
- Iterates through subtasks (respecting dependencies)
- Spawns child for each subtask
- Collects results
- Injects results as `CustomMessageEntry`
- Continues parent turn

#### Step 6: System Prompt Update ✅ (design)

Add decomposition instructions to system prompt. LLM learns to output JSON for multi-task requests.

#### Step 7: Config and CLI Flags ✅ (design)

Add `--subagent-enabled` flag (default: true). Add `--gpu-lock-path` for custom lock location.

## Future Considerations (Not in Phase 11)

- **Parallel sub-agents**: Allow multiple children when GPU resources permit
- **Inter-agent communication**: Sub-agents can read/write shared files during execution
- **Task graph visualization**: Display subtask DAG in TUI
- **Retry logic**: Retry failed subtasks automatically
- **Timeout per subtask**: Kill child if it runs too long
- **`/subtasks` command**: List active subtasks and their status
- **`/subtask <id>` command**: View details of a specific subtask

## Risks and Mitigations

| Risk | Mitigation |
|------|-----------|
| Child process hangs indefinitely | GPU semaphore timeout + max execution time per subtask |
| Child process fails silently | Check exit code, capture stderr |
| Result summary too short for context | Configurable result summary length (default 500 chars) |
| Too many subtasks overwhelm GPU | Sequential execution (one at a time) |
| Decomposition produces invalid JSON | Fallback to single subtask if JSON parse fails |
| Subtask dependencies create deadlock | Validate DAG before execution, detect cycles |

## Testing Strategy

### Unit Tests (no LLM)

- `subagent-test.cpp`: Test `GpuSemaphore` acquire/release, PID liveness check
- `session-entry-test.cpp`: Test `SubTaskEntry` JSON serialization/deserialization
- `decomposition-test.cpp`: Test JSON parsing, dependency validation, topological sort

### Integration Tests (local LLM)

- Test end-to-end: decompose → spawn → collect → inject
- Test with simple multi-task request
- Test with dependency graph (A → B → C)
- Test failure case (subtask fails, parent continues)

### Manual Testing

- Run `coding-agent --subagent-enabled` with multi-task prompt
- Verify child session files are created
- Verify results appear in parent session
- Verify GPU lock file is created/released

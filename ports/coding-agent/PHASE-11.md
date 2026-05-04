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

## Current Status

| Component | Status | Notes |
|-----------|--------|-------|
| `SubTaskEntry` / `SubTaskDecompositionEntry` | ✅ Implemented | In `session_entry.hpp`, variant updated |
| `GpuSemaphore` | ✅ Implemented | File-based lock, PID liveness, stale lock cleanup |
| `SubAgent::spawn()` | ✅ Implemented | fork/exec, GPU lock, result collection |
| `decomposeAndExecute()` | ✅ Implemented | Heuristic detection, LLM decomposition, sequential execution |
| `--gpu-lock-path` CLI flag | ✅ Implemented | Config field exists and is used at runtime |
| Dependency resolution | ✅ Implemented | `dependencies` field parsed and respected via topological sort |
| DAG validation / cycle detection | ✅ Implemented | Self-dependency, missing dependency, DFS cycle detection |
| Child process timeout | ✅ Implemented | Polling timeout loop, kill on timeout |
| `/decompose` command | ❌ Not implemented | Only heuristic-based detection |
| `/subtasks` command | ❌ Not implemented | |
| Parallel subagent execution | ❌ Not implemented | Sequential only |
| Multi-server GPU routing | ❌ Partially implemented | Single global lock file per server hash |
| Child session filename | ✅ Implemented | `subtask-<parent-session-id>-<subtask-id>_<timestamp>_<child-session-id>.jsonl` |
| Binary path resolution | ⚠️ Partial | Hardcoded `"coding-agent"` — should resolve from argv[0] |

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
- Expected artifacts are files the sub-agent is expected to create/modify
- If the request is a single task, respond with a single subtask
- If the request doesn't need decomposition, respond with a single subtask that handles the whole request
```

### Decomposition Decision Logic

The main agent decides whether to decompose based on heuristics (implemented in `looks_like_multi_task()`):

1. **Clause counting**: Counts distinct imperative clauses separated by commas, semicolons, or newlines. Multi-task if `clause_count >= 3` or `comma_count >= 2`.
2. **No explicit command**: There is no `/decompose` command — only heuristic detection.

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

## Server Connectivity

Sub-agents need to know which llama-cpp server to connect to. A server configuration module holds connectivity options that can be resolved at spawn time.

### Server Resolution

The main agent's llama-cpp connection details (host, port, model) are the default server configuration. A `ServerConfig` struct captures these options:

```cpp
struct ServerConfig {
  std::string baseUrl;       // e.g. "http://127.0.0.1:8080"
  std::string modelId;       // model used by parent
  std::string apiKey;        // if required
  std::vector<std::string> contextFiles;
};
```

### Default Behavior (Single Server)

When the llama-cpp provider has exactly **one server** in its servers list, or when no additional server resources are specified, the sub-agent uses **the same server** as the parent agent. The `ServerConfig` is inherited directly from the parent's connection:

- `baseUrl` — same as parent
- `modelId` — same as parent
- `apiKey` — same as parent
- `contextFiles` — copied from parent's session context

This means that in the common single-server case, the sub-agent invocation is nearly identical to running the agent in interactive mode (same binary, same model, same server), but with the added benefit of decomposition: the parent orchestrates multiple focused subtasks instead of one monolithic request.

### Multi-Server (Future)

When multiple servers are configured, the parent can route subtasks to different servers by specifying a `server` field in the subtask decomposition JSON. This is out of scope for Phase 11 but the `ServerConfig` module is designed to support it.

## Sub-Agent Spawn Logic

### Process Spawning

Each subtask spawns a child `coding-agent` process:

```cpp
struct SubAgentResult {
  std::string sessionId;        // Child session ID
  std::string sessionPath;      // Full path to child session file
  std::string lastAssistantMessage;  // Last assistant message content
  bool success = false;
  std::optional<std::string> error;
  int exitCode = -1;
};

class SubAgent {
 public:
  /// Spawn a child coding-agent process for a subtask.
  /// Blocks until the child completes.
  static SubAgentResult spawn(
      const std::string& binaryPath,
      const std::string& subtaskDescription,
      const std::vector<std::string>& contextFiles,
      const ServerConfig& serverConfig,
      const std::string& parentSessionDir,
      const std::string& gpuLockPath,
      int maxTokens,
      float temperature
  );

  /// Read the last assistant message from a session file.
  static std::string readLastAssistantMessage(const std::string& sessionPath);
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
  --session <child-session-path> \
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

  /// Check if the lock is currently held by another process.
  static bool isHeld(const std::string& lockPath);
};
```

**Implementation:** Write PID + timestamp to lock file. On acquire, check if PID is alive. If dead, stale lock cleaned up. If alive, wait with polling (100ms interval). On release, delete lock file.

### Child Session Organization

Child sessions are stored under the parent session's directory:

```
~/.config/coding-agent/sessions/
  └── sessions/
      └── --home-user-project--/
          ├── 2025-01-01T00:00:00_abc123.jsonl  (parent session)
          ├── subtask-abc123-1_1777869000000_x7f3a2b1c.jsonl  (child 1)
          ├── subtask-abc123-2_1777869000000_d4e5f6a7.jsonl  (child 2)
          └── subtask-abc123-3_1777869000000_9b8c7d6e.jsonl  (child 3)
```

**Format:** `subtask-<parent-session-id>-<subtask-id>_<timestamp>_<child-session-id>.jsonl`. This allows correlating child sessions with their parent and the specific subtask they executed.

## Result Collection

### Reading Sub-Agent Results

After the child process completes, the parent reads the last assistant message from the child session:

```cpp
std::string readLastAssistantMessage(const std::string& sessionPath) {
  // Load session entries from file
  // Walk from first to last line
  // Find last assistant message
  // Return content (up to 500 chars)
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

### Files Created

| File | Purpose |
|------|---------|
| `src/subagent.hpp` | SubAgent class, spawn logic, GPU semaphore |
| `src/subagent.cpp` | Implementation of spawn, result collection |
| `src/gpu_semaphore.hpp` | GpuSemaphore class (file-based lock) |
| `src/gpu_semaphore.cpp` | Implementation of GPU semaphore |

### Files Modified

| File | Changes |
|------|---------|
| `src/session_entry.hpp` | Added `SubTaskEntry`, `SubTaskDecompositionEntry` to variant |
| `src/agent_session.hpp` | Added `decomposeAndExecute()`, `decomposeIntoSubtasks()`, `executeSubtasks()` |
| `src/agent_session.cpp` | Implemented decomposition logic, child spawn, result collection |
| `src/config.hpp` | Added `gpu_lock_path` config field |
| `src/config.cpp` | Added `--gpu-lock-path` CLI flag parsing |

### Implementation Steps

#### Step 1: Session Entry Types ✅ Implemented

Added `SubTaskEntry` and `SubTaskDecompositionEntry` to the session entry variant. JSON serialization/deserialization handled by nlohmann::json.

#### Step 2: GPU Semaphore ✅ Implemented

File-based GPU lock with PID liveness check. Stale lock cleanup on read. Polling wait with 100ms interval. Timeout configurable (default 60s, used as 120s in spawn).

#### Step 3: SubAgent Spawn ✅ Implemented

`SubAgent::spawn()` acquires GPU lock (waits with 120s timeout), constructs child command line, spawns via fork/exec (Unix) or CreateProcess (Windows), waits for completion, collects exit code and session file path, releases GPU lock.

#### Step 4: Result Collection ✅ Implemented

Reads last assistant message from child session file by parsing JSONL lines. Returns first 500 characters as summary.

#### Step 5: AgentSession Integration ✅ Implemented (partial)

`decomposeAndExecute()` detects multi-task requests via heuristic (`looks_like_multi_task()`), calls LLM to decompose, creates `SubTaskDecompositionEntry`, spawns children sequentially, collects results, injects as `CustomMessageEntry`. **Dependencies field is ignored** — subtasks always execute in array order.

#### Step 6: System Prompt Update ✅ Implemented

Decomposition instructions added to system prompt via `decomposeIntoSubtasks()` prompt construction.

#### Step 7: Config and CLI Flags ✅ Implemented

`--gpu-lock-path` flag added and used at runtime. If set, that path is used; otherwise, the lock path is auto-derived from a hash of `base_url` to prevent conflicts on multi-GPU setups.

#### Step 8: DAG Validation & Dependency Resolution ✅ Implemented

- `validateSubtaskDag()`: Validates that the dependency graph has no cycles, no self-dependencies, and no references to unknown task IDs.
- `topologicalSortSubtasks()`: Computes execution order using Kahn's algorithm with priority-based tie-breaking. Returns `nullopt` if the graph is invalid.
- Subtasks are now executed in topological order, respecting the `dependencies` field from the LLM output.

#### Step 9: Child Session Filenames ✅ Implemented

Child session files now follow the format: `subtask-<parent-session-id>-<subtask-id>_<timestamp>_<child-session-id>.jsonl`. This makes it easy to correlate child sessions with their parent and the specific subtask they executed.

#### Step 10: Unit Tests ✅ Implemented

`coding-agent-subagent-dag-test` covers:
- DAG validation: empty graph, single task, linear deps, diamond deps, self-dependency, missing dependency, cycles of 2 and 3
- Topological sort: single task, linear order, diamond order, priority ordering, independent tasks, cycle rejection

## Issues and Architecture Concerns

### 1. GPU Lock File — Per-Server Locks ✅ Fixed

**Previous behavior:** The GPU lock was a single file at `<cwd>/.pi/gpu.lock`. All subagents on the same machine fought over this one lock.

**Fix:** Lock path is now auto-derived from a hash of `base_url` when `--gpu-lock-path` is not explicitly set. This prevents lock conflicts when running subagents on different machines connecting to different GPUs.

**Remaining:** Cross-machine locks (e.g., Redis, NFS) are still not supported.

### 2. `config_.gpu_lock_path` Is Now Used ✅ Fixed

The config field is parsed from `--gpu-lock-path` CLI flag and used at runtime in `executeSubtasks()`. Falls back to `base_url` hash if not set.

### 3. Dependencies Field Is Now Respected ✅ Fixed

The LLM can produce `dependencies: ["1"]` in the JSON, and `executeSubtasks()` now:
- Validates the DAG with `validateSubtaskDag()` (checks for cycles, self-deps, missing deps)
- Computes topological execution order with `topologicalSortSubtasks()` (Kahn's algorithm with priority tie-breaking)
- Executes subtasks in that order instead of array order

### 4. DAG Validation & Cycle Detection ✅ Implemented

`validateSubtaskDag()` checks for:
- Self-dependencies (task depends on itself)
- Missing dependencies (task depends on unknown task ID)
- Cycles (DFS-based cycle detection)

`topologicalSortSubtasks()` uses Kahn's algorithm and returns `nullopt` if the graph is invalid.

### 5. Child Process Timeout ✅ Implemented

`SubAgent::spawn()` uses a polling loop with `waitpid(WNOHANG)` (Unix) or `WaitForSingleObject` (Windows) and a configurable `maxSubtaskDurationMs` parameter (default 30 minutes). On timeout, the child process is killed and an error is reported.

### 6. Binary Path Resolution ⚠️ Partial

`std::string binaryPath = "coding-agent";` — the child is launched via `execv("coding-agent", ...)` which assumes `coding-agent` is in PATH. Should resolve from `argv[0]` or a config option for reliability.

### 7. Child Session Filename Now Matches Spec ✅ Fixed

Child session files now follow the format: `subtask-<parent-session-id>-<subtask-id>_<timestamp>_<child-session-id>.jsonl`. This makes it easy to correlate child sessions with their parent and find the correct subtask result.

### 8. UI: TuiAnimation State Confusion During Subtask Execution

**Current behavior:** When `decomposeAndExecute()` runs, it produces chunks like `[DECOMPOSE]`, `[SUBTASK 1/3]`, `[tool: subtask_1] completed`. The `on_chunk` callback in `interactive_mode.cpp` calls `animation.on_first_stream_chunk()` and `animation.update(AnimationState::Generating, "")` on the first chunk.

**Problem:** The user sees the "generating" animation (thinking dots) while subagents are actually running. No LLM text is streaming from the parent — it's just orchestration output. The animation state does not distinguish between "parent is generating text" and "parent is waiting for a subagent."

**UI implications for the first coding-agent spawned:**
1. The user types a multi-task request.
2. The parent shows `[DECOMPOSE] Detecting subtasks...` — animation starts.
3. LLM call for decomposition happens (no visible output during the call).
4. `[DECOMPOSE] Found N subtask(s)` — user sees this.
5. `[EXECUTE] Starting subtask execution...` — user sees this.
6. `[SUBTASK 1/N] Implement auth module` — animation continues showing "generating".
7. The child process runs (potentially minutes) — user sees nothing new until the child completes.
8. `[tool: subtask_1] completed` — result appears.

The user has no way to see the child's progress. The child's TUI output is discarded (child runs in non-interactive mode via `--new-session` with the prompt as argument, no readline loop). The only visibility is the parent's `on_chunk` output.

**What should happen:**
- Show `[SUBTASK 1/N] Running... (waiting for completion)` with a distinct state (not "generating").
- Optionally stream the child's output back to the parent via a pipe, so the user can see tool calls and file edits in real-time.
- Add a `/subtasks` command to list subtask status and results.

### 9. No `/decompose` Command

The system prompt mentions `/decompose` as an explicit trigger, but it is not implemented. Decomposition is purely heuristic-based via `looks_like_multi_task()`.

### 10. No `/subtasks` Command

There is no way to list active subtasks, view their status, or see results without parsing the session file.

## Future Considerations (Not in Phase 11)

- **Parallel sub-agents**: Allow multiple children when GPU resources permit (requires multi-GPU lock support)
- **Inter-agent communication**: Sub-agents can read/write shared files during execution
- **Task graph visualization**: Display subtask DAG in TUI
- **Retry logic**: Retry failed subtasks automatically
- **Timeout per subtask**: Kill child if it runs too long (e.g., 30 min)
- **`/subtasks` command**: List active subtasks and their status
- **`/subtask <id>` command**: View details of a specific subtask
- **`/decompose` command**: Explicitly trigger decomposition
- **Child output streaming**: Pipe child stdout/stderr back to parent for real-time visibility
- **Cross-machine GPU locks**: Network-based semaphore for multi-server setups

## Risks and Mitigations

| Risk | Mitigation | Status |
|------|-----------|--------|
| Child process hangs indefinitely | GPU semaphore timeout + max execution time per subtask | ✅ Implemented |
| Child process fails silently | Check exit code, capture stderr | ✅ Implemented |
| Result summary too short for context | Configurable result summary length (default 500 chars) | ✅ Implemented |
| Too many subtasks overwhelm GPU | Sequential execution (one at a time) | ✅ Implemented |
| Decomposition produces invalid JSON | Fallback to single subtask if JSON parse fails | ✅ Implemented |
| Subtask dependencies create deadlock | Validate DAG before execution, detect cycles | ✅ Implemented |
| GPU lock blocks across machines | File-based lock does not work cross-machine | ❌ Not addressed |
| No visibility into child progress | Child output not streamed to parent | ❌ Not implemented |
| Binary not in PATH | Hardcoded "coding-agent" string | ⚠️ Partial |

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
- Test with two separate llama-cpp servers (different GPUs) — **currently broken**

# PHASE-12: RPC Mode (JSONL-based headless/remote protocol)

## Goal

Add a headless RPC mode to the C++ coding agent so that a higher-level agent orchestrator can manage the C++ agent remotely via a JSONL stdin/stdout protocol. This mirrors the existing TypeScript RPC mode (`packages/coding-agent/src/modes/rpc/`) and enables embedding the C++ agent in automated pipelines, multi-agent systems, and remote workloads.

## Protocol Overview

- **Commands**: JSON lines on stdin. Each command has a `type` field and an optional `id` field for request-response correlation.
- **Responses**: JSON lines on stdout with `type: "response"`, `command`, `success`, and optional `data`/`error`.
- **Events**: AgentSession events streamed on stdout as they occur (same event objects as TS RPC mode).
- **Extension UI**: Extension UI requests emitted to stdout; client responds via `extension_ui_response` on stdin.
- **Framing**: LF-only JSONL. Payload strings may contain Unicode separators. Clients must split on `\n` only.

## Command Set

### Prompting

| Command | Description |
|---------|-------------|
| `prompt` | Send a user message and start the agent loop. |
| `steer` | Queue a steering message (interrupt mid-run, delivered before next LLM call). |
| `follow_up` | Queue a follow-up message (delivered when agent is idle). |
| `abort` | Abort the current operation. |
| `new_session` | Create a new session, optionally with parent tracking. |

### State

| Command | Description |
|---------|-------------|
| `get_state` | Return current session state (model, thinking level, streaming status, etc.). |

### Model

| Command | Description |
|---------|-------------|
| `set_model` | Set model by provider/model ID. |
| `cycle_model` | Cycle to next available model. |
| `get_available_models` | List all available models. |

### Thinking

| Command | Description |
|---------|-------------|
| `set_thinking_level` | Set thinking level (off/minimal/low/medium/high/xhigh). |
| `cycle_thinking_level` | Cycle to next thinking level. |

### Queue Modes

| Command | Description |
|---------|-------------|
| `set_steering_mode` | Set steering queue mode (`all` / `one-at-a-time`). |
| `set_follow_up_mode` | Set follow-up queue mode (`all` / `one-at-a-time`). |

### Compaction

| Command | Description |
|---------|-------------|
| `compact` | Manually trigger compaction (optional `custom_instructions`). |
| `set_auto_compaction` | Enable/disable auto-compaction. |

### Retry

| Command | Description |
|---------|-------------|
| `set_auto_retry` | Enable/disable auto-retry. |
| `abort_retry` | Abort in-progress retry. |

### Bash

| Command | Description |
|---------|-------------|
| `bash` | Execute a shell command. |
| `abort_bash` | Abort the running bash command. |

### Session

| Command | Description |
|---------|-------------|
| `get_session_stats` | Return session statistics. |
| `switch_session` | Switch to a different session file. |
| `fork` | Fork from a specific entry ID. |
| `clone` | Clone the current branch into a new session. |
| `get_fork_messages` | Get messages available for forking. |
| `get_last_assistant_text` | Get text of last assistant message. |
| `set_session_name` | Set the session display name. |

### Messages

| Command | Description |
|---------|-------------|
| `get_messages` | Return all messages in the session. |

## Response Format

```json
{ "id": "req_1", "type": "response", "command": "prompt", "success": true }
{ "id": "req_1", "type": "response", "command": "prompt", "success": false, "error": "..." }
{ "id": "req_1", "type": "response", "command": "get_state", "success": true, "data": { ... } }
```

- `id`: Echoed from the command if present, or `null` for fire-and-forget.
- `type`: Always `"response"` for command responses.
- `command`: The original command type.
- `success`: `true` or `false`.
- `data`: Present on success (command-dependent).
- `error`: Present on failure.

## Event Format

Events emitted on stdout are the same `AgentEvent` objects from the C++ session. Each is a JSON line:

```json
{ "type": "turn_start", "turn_index": 1 }
{ "type": "tool_call", "tool_name": "bash", "tool_call_id": "abc123", "tool_args": "ls -la" }
{ "type": "tool_execution_start", "tool_name": "bash", "tool_call_id": "abc123" }
{ "type": "tool_execution_update", "tool_name": "bash", "tool_call_id": "abc123", "partial_result": "total 42" }
{ "type": "tool_execution_end", "tool_name": "bash", "tool_call_id": "abc123", "tool_result": "total 42", "tool_error": false }
{ "type": "turn_end", "turn_index": 1 }
{ "type": "model_call_start", ... }
```

## Extension UI

Extension UI requests are emitted when extensions need user input:

```json
{ "type": "extension_ui_request", "id": "uuid", "method": "select", "title": "Choose", "options": ["a", "b"], "timeout": 30000 }
```

The client responds:

```json
{ "type": "extension_ui_response", "id": "uuid", "value": "a" }
```

Methods: `select`, `confirm`, `input`, `editor`, `notify`, `setStatus`, `setWidget`, `setTitle`, `set_editor_text`.

## Session State Object

Returned by `get_state`:

```json
{
  "model": { "provider": "llama-cpp", "id": "llama3.1-8b", "contextWindow": 128000, "reasoning": false },
  "thinkingLevel": "medium",
  "isStreaming": false,
  "isCompacting": false,
  "steeringMode": "all",
  "followUpMode": "all",
  "sessionFile": "/path/to/session.jsonl",
  "sessionId": "abc-def-ghi",
  "sessionName": "My Session",
  "autoCompactionEnabled": true,
  "messageCount": 24,
  "pendingMessageCount": 0
}
```

## Implementation Plan

### Phase 12.1: Core Infrastructure

**Files to create:**

- `src/modes/rpc_mode.cpp` / `src/modes/rpc_mode.hpp` — RPC mode implementation
- `src/jsonl.cpp` / `src/jsonl.hpp` — JSONL reader/writer (LF-only framing)

**Implementation details:**

1. **JSONL reader**: Implement a C++ JSONL line reader (similar to the TS `attachJsonlLineReader`). Read from `stdin`, split on `\n`, parse each line with nlohmann/json. Handle partial reads and buffering correctly.

2. **JSONL writer**: Simple `writeRawStdout(serializeJsonLine(obj))` — just `JSON.stringify(obj) + "\n"` to stdout. No buffering needed beyond what `std::cout` provides (use `std::cout << ... << '\n'` with `std::endl` or flush explicitly).

3. **Command dispatch**: A `handleCommand()` function that switches on `type` and calls corresponding `AgentSession` methods. Responses are emitted via `output()`.

4. **Event subscription**: Register an event handler via `AgentSession::set_event_handler()` that serializes and emits each event to stdout.

### Phase 12.2: Command Implementations

Map each RPC command to the corresponding `AgentSession` method:

| RPC Command | C++ AgentSession Method |
|-------------|------------------------|
| `prompt` | `run()` |
| `steer` | `steer()` |
| `follow_up` | `followUp()` |
| `abort` | `abort()` |
| `new_session` | `createNewSession()` + `switchSession()` |
| `get_state` | Direct accessors on `AgentSession` |
| `set_model` | `set_model()` |
| `cycle_model` | `cycle_model()` |
| `get_available_models` | N/A (static model — return the configured model) |
| `set_thinking_level` | `set_thinking_level()` |
| `cycle_thinking_level` | `cycle_thinking_level()` |
| `set_steering_mode` | `set_steering_mode()` |
| `set_follow_up_mode` | `set_follow_up_mode()` |
| `compact` | `compact()` |
| `set_auto_compaction` | N/A (expose via config or state) |
| `set_auto_retry` | N/A (expose via config or state) |
| `abort_retry` | N/A (no retry state in C++ yet) |
| `bash` | `BashTool::execute()` |
| `abort_bash` | `set_bash_cancel_flag()` |
| `get_session_stats` | `session_id()`, `compaction_count()`, `total_context_tokens()`, `message_count()` |
| `switch_session` | `switchSession()` |
| `fork` | `branchFrom()` |
| `clone` | `branch()` |
| `get_fork_messages` | N/A (return messages from session) |
| `get_last_assistant_text` | Access last assistant message from `messages()` |
| `set_session_name` | N/A (add to SessionManager) |
| `get_messages` | `messages()` |

### Phase 12.3: Config & CLI Integration

- Add `--mode rpc` CLI flag to `Config` parsing.
- In `agent.cpp`, select `runRpcMode()` when `config.print_mode == false && config.rpc_mode == true` (or a new `mode` enum: `interactive`, `print`, `rpc`).
- RPC mode inherits all config options (model, provider, tools, etc.) from CLI args and `settings.json`.

### Phase 12.4: Event Serialization

Define the JSON schema for each `AgentEvent` variant and implement a serializer. The C++ `AgentEvent` struct maps directly to the TS event format. Use nlohmann/json's `to_json` / `from_json` to handle serialization.

### Phase 12.5: Extension UI Support (Out of Scope for Phase 12)

The C++ port has no extension system. Extension UI methods will be stubbed as no-ops or fire-and-forget events, similar to the TS RPC mode's approach. Extension-specific commands (`get_commands`) will return empty lists.

## C++ vs TS Protocol Differences

| Aspect | TS RPC | C++ RPC |
|--------|--------|---------|
| Provider discovery | Dynamic via `ModelRegistry` | Static (single configured model) |
| `get_available_models` | Returns all discovered models | Returns only the configured model |
| Auto-compaction/retry config | Runtime toggle | Config-time only (via `settings.json` or CLI) |
| Extension system | Full support | Stubbed (no extensions in C++) |
| Image content | Supported in prompt/steer/follow_up | Not yet supported (no `ImageContent` in C++ messages) |
| Session lifecycle | Full `AgentSessionRuntime` with switch/fork/clone | Partial (has `switchSession`, `createNewSession`, `branch`, `branchFrom`) |
| Bash execution | Via `BashOperations` abstraction | Inline in `BashTool` (no abstraction yet) |
| Session import | `importFromJsonl` | Not yet implemented |
| Session export (HTML) | `exportToHtml` | Not implemented |
| Fork with editor | Interactive editor for text extraction | `fork` returns entry text directly |

## Design Decisions

1. **Single binary, mode selection**: Add `--mode rpc` alongside `--mode print` and the default interactive mode. No separate binary.

2. **nlohmann/json for serialization**: Use the existing nlohmann/json dependency for JSON parsing/serialization. No new dependencies.

3. **Blocking run() for prompt**: The `prompt` command triggers `AgentSession::run()`, which blocks until the turn completes. Events stream concurrently on stdout. This matches the TS behavior where `prompt` returns immediately and events follow.

4. **No extension system**: Extension UI requests are emitted as stubs (fire-and-forget). `get_commands` returns an empty list. This is acceptable since the C++ port has no extension infrastructure.

5. **Image support deferred**: Since C++ `ChatMessage` content is plain string (no multi-part content), image support in RPC commands will be ignored or rejected with a clear error until image content is added to the C++ port.

6. **Auto-compaction/retry**: These are currently set in `AgentSessionConfig` at construction time. Add runtime setters (`setAutoCompactionEnabled`, `setAutoRetryEnabled`) to `AgentSession` to support the RPC commands.

## File Layout

```
ports/coding-agent/src/
├── jsonl.hpp               # JSONL reader/writer
├── jsonl.cpp
├── modes/
│   ├── rpc_mode.hpp        # RPC mode entry point
│   └── rpc_mode.cpp
├── agent.cpp               # Add --mode rpc to CLI parsing
├── agent_session.hpp       # Add setAutoCompactionEnabled, setAutoRetryEnabled
└── agent_session.cpp
```

## Testing

- Unit tests for JSONL reader/writer (edge cases: partial reads, Unicode, trailing newlines).
- Integration test: spawn the C++ agent in RPC mode, send commands, verify responses and events.
- Test all command types and error cases.
- Test event ordering (events should be emitted in the order they occur, interleaved with responses).

## Dependencies

- nlohmann/json (already a dependency for session JSONL storage).
- No new dependencies required.

# Phase 7: AgentSession Runtime Integration

## Goal

`agent_session.hpp` declares a complete `AgentSession` API but has no implementation — the
runtime is still driven by the free function `run_agent_loop` in `agent_loop.cpp`.  Phase 7
implements `AgentSession`, wires it into both modes, and removes `agent_loop`.

## Complexity: Medium

## Prerequisites

- Phase 6 complete: cancellation + TUI animation working.
- Build passes with zero errors and zero new warnings before starting.

---

## Current State of the Port (as of Phase 7 start)

### Core infrastructure (implemented)

| Area | Status |
|------|--------|
| LlamaCpp provider (HTTP, streaming, cancel via `cancel_flag`) | Done |
| `SessionStore` (JSONL, branch tree via `SessionGraph`) | Done |
| Tools: bash, edit, find, grep, ls, read, write | Done |
| `agent_loop.cpp` — free-function agent loop | Done (to be replaced) |
| `compaction.cpp` — threshold check + `compact_history` | Done |
| `branch_summary.cpp` — `generate_branch_summary` + `collect_entries_for_branch_summary` | Done |
| `interactive_mode.cpp` — readline REPL, `/compact`, `/stats`, TUI animation, Ctrl+C | Done |
| `print_mode.cpp` — one-shot print mode | Done |
| `context_loader.cpp` — `.context` file loading | Done |
| `system_prompt.cpp` — system prompt building | Done |
| `config.cpp` — CLI argument parsing, settings.json support | Done |

### What is only declared (no implementation)

| Area | Notes |
|------|-------|
| `AgentSession` class (`agent_session.hpp`) | Full API declared; **no `agent_session.cpp` exists** |
| `ThinkingLevel::XHigh` | Missing from enum (only `Off`…`High`) |
| `thinking_level_to_string` / `string_to_thinking_level` | Declared, not implemented |
| `AgentSession::run()`, `set_model()`, `compact()`, event handler | All declared, none wired |

### Missing from the port (deferred to later phases)

| Priority | Area |
|----------|------|
| 1 | **SessionManager tree model** — full branch/leaf traversal, labels, typed entries |
| 2 | **Extension system** — ExtensionRunner, hooks, extension commands |
| 3 | **AuthStorage + OAuth** — credential backends, OAuth flows |
| 4 | **ModelRegistry** — provider registration, API-key model discovery |
| 5 | **Queue management + retry** — steer/followUp queues, exponential backoff |
| 6 | **Interactive UI parity** — selector/dialog/component surface |
| 7 | **Theme system** — colors, code highlighting |
| 8 | **RPC mode** — JSONL RPC server/client |
| 9 | **Session export** — HTML/JSONL export |
| 10 | **Prompt templates + skills** — template expansion, `/skill` commands |
| 11 | **SDK factories** — programmatic `createAgentSession` etc. |

---

## Phase 7 Scope

Implement `AgentSession` as the canonical runtime unit and remove `agent_loop`.

| # | Task | Status |
|---|------|--------|
| 1 | Add `XHigh` to `ThinkingLevel` enum | Done |
| 2 | Add `ModelCallStart` event type (for between-tool-round animation) | Done |
| 3 | Add `session_config()` getter and `active_tool_definitions()` private method to header | Done |
| 4 | Implement `agent_session.cpp` (full runtime: constructor, `run()`, tools, compaction, events) | Done |
| 5 | Refactor `agent.cpp` to create `AgentSession` and pass it to modes | Done |
| 6 | Refactor `interactive_mode` to use `AgentSession` (events drive animation, new commands) | Done |
| 7 | Refactor `print_mode` to use `AgentSession` | Done |
| 8 | Delete `agent_loop.hpp` / `agent_loop.cpp` | Done |
| 9 | Build clean with `-Werror` | Done |

---

## Tasks

### 1–3. Header changes (`agent_session.hpp`)

- [x] `ThinkingLevel::XHigh` added to enum after `High`.
- [x] `AgentEvent::Type::ModelCallStart` added — fires before each provider call within a turn
  (used by the interactive mode to resume the TUI animation between tool rounds).
- [x] `const AgentSessionConfig& session_config() const` public getter.
- [x] `std::vector<ToolDefinition> active_tool_definitions() const` private helper.

---

### 4. `agent_session.cpp`

**Constructor**

- [x] Parse `config.initial_active_tools` (comma-separated) into `active_tool_names_`.
- [x] Call `session_.load_messages()` to populate `messages_`.
- [x] If no system message present: call `load_context_files` + `build_system_prompt`, prepend
  and persist via `session_.append()`.

**`run(user_input, on_chunk, cancel_flag)`**

- [x] Guard against re-entrant calls (`running_` atomic).
- [x] Reset `abort_requested_`; call `run_turn()`; clear `running_` on exit.

**`run_turn()` — inner loop**

- [x] Append user `ChatMessage` with `session_.assign_entry_id()` to `messages_` and session.
- [x] Emit `TurnStart` event.
- [x] For each iteration up to `max_tool_iterations`:
  - Emit `ModelCallStart` (drives between-tool animation).
  - Call `call_provider()`.
  - On interrupt: roll back user message, return false.
  - Append assistant message with entry id to `messages_` and session.
  - Emit per-turn token breakdown via `on_chunk`.
  - Call `check_and_compact()`.
  - If no tool calls: break.
  - Call `execute_tools()`.
- [x] Increment `turn_index_`; emit `TurnEnd`.

**`execute_tools()`**

- [x] For each tool call: emit `ToolCall` event, call `on_chunk` with start/done lines,
  dispatch via `tools_.dispatch()`, emit `ToolResult` event, append tool `ChatMessage`.

**`check_and_compact()`**

- [x] `should_compact()` guard.
- [x] Emit `CompactionStart`; call `compact_history()`; emit `CompactionEnd`.
- [x] On success: persist `CompactionEvent`, update `last_compaction_stats_` + `compaction_count_`.
- [x] On failure + `compaction_fail_fast`: return false; otherwise gracefully persist
  `compaction_skipped` row and continue.

**`compact()` (manual)**

- [x] Same compaction path as `check_and_compact` but always runs regardless of threshold.

**Model / thinking level management**

- [x] `set_model()`: update `current_model_` + `config_.model`, emit `ModelChange` event.
- [x] `cycle_model()`: stub (no model list in Phase 7; returns false).
- [x] `set_thinking_level()` / `cycle_thinking_level()`: update state, emit `ThinkingLevelChange`.
- [x] `available_thinking_levels()`: returns all six levels including `XHigh`.

**Event emission**

- [x] `emit_event()`: call `event_handler_` if set.
- [x] `set_event_handler()`: store handler.

**State accessors**

- [x] `model()`, `thinking_level()`, `system_prompt()`, `messages()`, `message_count()`.
- [x] `active_tools()`, `all_tool_definitions()`.
- [x] `session_id()`, `session_path()`, `compaction_count()`, `total_context_tokens()`.
- [x] `last_compaction_stats()`, `is_compacting()`, `is_running()`.

---

### 5. `agent.cpp` refactor

- [x] Remove history loading and system-prompt setup (moved to `AgentSession` constructor).
- [x] Build `AgentSessionConfig` from `Config`.
- [x] Create `AgentSession agent(cfg, provider, tools, session)`.
- [x] Pass `agent` (and `config.prompt` where needed) to mode functions.

---

### 6. `interactive_mode` refactor

New signature: `int run_interactive_mode(AgentSession& agent)`.

- [x] Event handler registered on `agent`: `ModelCallStart` → `animation.resume_for_next_model_turn()`.
- [x] `on_chunk` callback: unchanged (calls `animation.on_first_stream_chunk()`, streams text).
- [x] Per-turn: call `agent.run(prompt, on_chunk, &global_cancel_flag)`.
- [x] Token budget display uses `agent.total_context_tokens()` and `agent.session_config()`.
- [x] `/compact`: call `agent.compact()` + display result.
- [x] `/stats`: use `agent.compaction_count()`, `agent.message_count()`, `agent.session_id()`, etc.
- [x] Config-dependent display values (`context_size`, `compaction_reserve_tokens`) accessed via
  `agent.session_config()`.

---

### 7. `print_mode` refactor

New signature: `int run_print_mode(AgentSession& agent, const std::string& prompt)`.

- [x] Read stdin or use provided prompt.
- [x] Call `agent.run(prompt, on_chunk)`.

---

### 8. Remove `agent_loop`

- [x] Delete `src/agent_loop.hpp`.
- [x] Delete `src/agent_loop.cpp`.

---

## Validation

```bash
# Build
cmake -S ports/coding-agent -B ports/coding-agent/build
cmake --build ports/coding-agent/build -j

# Tests
cd ports/coding-agent/build && ctest --output-on-failure

# Smoke test interactive
ports/coding-agent/build/coding-agent --base-url http://beugul-desktop:8080
# → TUI animation, Ctrl+C, /compact, /stats all work as before

# Print mode
ports/coding-agent/build/coding-agent --base-url http://beugul-desktop:8080 --prompt "say hi"
```

## Acceptance Criteria

- [x] `AgentSession` implemented and used exclusively; `agent_loop` deleted.
- [x] `ThinkingLevel::XHigh` present in enum and cycling.
- [x] `ModelCallStart` event drives between-tool-round animation in interactive mode.
- [x] All existing interactive commands (`/compact`, `/stats`, `/tokens`, `/clear`, `/exit`) work.
- [x] Ctrl+C cancellation unchanged.
- [x] TUI animation unchanged.
- [x] Build clean with `-Werror`.
- [ ] `cycle_model()` returns a real model list (deferred — no model list in Phase 7).

---

## Missing Functionality Catalog (from TypeScript original)

The remainder of this file catalogs TypeScript `packages/coding-agent/src` features not yet
ported.  This replaces the old gap table and will be updated as future phases complete work.

---

### Agent Session (agent-session.ts)

| Feature | TypeScript | C++ Port |
|---------|-----------|----------|
| AgentSession class | Full lifecycle | Implemented in Phase 7 |
| Event system (subscribe, _handleAgentEvent, _emit) | Yes | Partial (single handler, no multi-subscriber) |
| Agent state (state, model, thinkingLevel, isStreaming, systemPrompt) | Yes | Done |
| Model management (setModel, cycleModel, cycleThinkingLevel) | Yes | Partial (`cycleModel` stub — no model list) |
| ThinkingLevel: off…xhigh | Yes | Done (added XHigh in Phase 7) |
| Queue management (steer, followUp, clearQueue) | Yes | No |
| Auto-compaction | Yes | Done |
| Branch summarization (navigateTree, generateBranchSummary) | Yes | Partial (no interactive tree navigation) |
| Auto-retry with exponential backoff | Yes | No |
| Session name management | Yes | No |
| Session stats (getSessionStats) | Yes | Partial |
| HTML/JSONL export | Yes | No |
| Custom messages (sendCustomMessage) | Yes | No |
| User messages with images | Yes | No |
| Prompt template expansion | Yes | No |
| Skill command expansion (/skill:name) | Yes | No |
| Extension system integration | Yes | No |
| Tool registry management (getActiveToolNames, setActiveToolsByName) | Yes | Done |
| Context usage tracking (getContextUsage) | Yes | Partial |
| Model cycling with scoped models | Yes | No |

---

### Session Manager (session-manager.ts)

| Feature | TypeScript | C++ Port |
|---------|-----------|----------|
| SessionManager class | Full tree traversal, branching | No (`SessionGraph` supports traversal; no manager class) |
| Session entry types (9+ types) | Yes | Partial (`message`, `compaction`, `branch_summary`, `compaction_skipped`) |
| Branch/leaf management | Yes | No |
| Labels on entries | Yes | No |
| Session migration | Yes | No |
| Session context building | Yes | No |
| Version tracking | Yes | No |

---

### Auth Storage (auth-storage.ts)

| Feature | TypeScript | C++ Port |
|---------|-----------|----------|
| AuthStorage class | API key + OAuth | No |
| FileAuthStorageBackend | Yes | No |
| InMemoryAuthStorageBackend | Yes | No |
| OAuth credential management | Yes | No |

---

### Model Registry (model-registry.ts)

| Feature | TypeScript | C++ Port |
|---------|-----------|----------|
| ModelRegistry class | API key resolution, discovery | No |
| Provider registration/unregistration | Yes | No |
| OAuth detection | Yes | No |

---

### Settings Manager (settings-manager.ts)

| Feature | TypeScript | C++ Port |
|---------|-----------|----------|
| SettingsManager class | Full settings persistence | No |
| Compaction settings | Yes | Partial (via Config) |
| Retry settings | Yes | No |
| Default thinking level persistence | Yes | No |
| Theme persistence | Yes | No |

---

### Compaction (core/compaction/)

| Feature | TypeScript | C++ Port |
|---------|-----------|----------|
| compact() | Full with cut-point finding | Partial |
| findCutPoint | Smart cut-point algorithm | No |
| calculateContextTokens / estimateContextTokens | Yes | Partial |
| shouldCompact | Yes | Done |
| generateBranchSummary | Yes | Partial |
| Compaction failure handling | Yes | Done |

---

### Extension System (core/extensions/)

Entirely absent from the port.

---

### Interactive Mode Components (modes/interactive/components/)

The C++ port has a readline-based REPL, but none of the TypeScript component architecture.

| Notable gap | Notes |
|-------------|-------|
| Selector/dialog/component surface | No |
| Model selector | No (`cycle_model` stub only) |
| Theme selector | No |
| Session tree navigator | No |
| Login dialog | No |

---

### Theme System (modes/interactive/theme/)

Entirely absent from the port.

---

### RPC Mode (modes/rpc/)

Entirely absent from the port.

---

### CLI (cli/)

| Feature | TypeScript | C++ Port |
|---------|-----------|----------|
| args.ts | Comprehensive argument parsing | Partial (config.cpp) |
| list-models.ts | Model listing | No |
| session-picker.ts | Session picker | No |

---

### Core Utilities (core/)

| Feature | TypeScript | C++ Port |
|---------|-----------|----------|
| event-bus.ts | Event bus | No |
| prompt-templates.ts | Prompt template expansion | No |
| skills.ts | Skills loading | No |
| slash-commands.ts | Slash commands | No |
| resource-loader.ts | Resource loading | Partial (context_loader.cpp) |
| system-prompt.ts | System prompt building | Partial (system_prompt.cpp) |

---

### SDK (core/sdk.ts)

Entirely absent from the port.

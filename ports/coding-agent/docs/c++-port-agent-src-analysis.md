# C++ port vs `packages/coding-agent` — analysis and roadmap

Primary TypeScript reference for the agent loop: **`packages/coding-agent/src/core/agent-session.ts`** (`AgentSession`, `AgentSessionRuntime`). The C++ binary lives under **`ports/coding-agent`**.

## Architecture

| Aspect | TS (`packages/coding-agent`) | C++ (`ports/coding-agent`) |
|--------|------------------------------|---------------------------|
| **Product boundary** | SDK library + CLI (embeddable) | Standalone CLI binary |
| **Runtime** | Node.js / Bun | Native C++17 |
| **Agent core** | `AgentSession` + `AgentSessionRuntime` (session lifecycle) | `AgentSession` (loop only) |
| **Provider** | Multi-provider (`pi-ai` package) | Single provider (`llama-cpp`) |
| **Session store** | JSONL with `SessionManager` | JSONL with `SessionManager` |
| **Extension system** | Full plugin system | None |
| **Event granularity** | `tool_execution_start`/`update`/`end`, `turn_start`/`end`, `message_start`/`update`/`end` | `ToolExecutionStart`/`Update`/`End`, `ToolCall`/`ToolResult`, `TurnStart`/`TurnEnd`, `ModelCallStart` |

## TypeScript (`packages/coding-agent/src/core`)

- **`AgentSession`** — stateful wrapper: events, steering/follow-up queues, prompt/continue, abort, compaction, retry, model/thinking management, branching, custom messages, bash execution, session export.
- **`AgentSessionRuntime`** — session lifecycle: `switchSession`, `newSession`, `fork`, `importFromJsonl`, teardown/rebind pattern.
- **`AgentSessionServices`** — cwd-bound service factory: auth, settings, model registry, resource loader.
- **`extensions/`** — full plugin system: `ExtensionRunner`, core bindings, UI context, command system, resource discovery, hooks (`tool_call`, `tool_result`, `session_before_compact`, `session_before_tree`, `session_before_switch`, `session_before_fork`, `session_shutdown`, `session_start`, `input`, `resources_discover`).
- **`tools/`** — built-in tools: bash, edit, find, grep, ls, read, write.
- **`modes/`** — interactive (TUI with rich components), print, RPC (JSONL).
- **`session-manager.ts`** — JSONL session store with tree traversal, branching, compaction/branch summary entries.
- **`settings-manager.ts`** — persistent settings (model, thinking level, compaction, retry, shell config, themes).
- **`model-registry.ts`** — dynamic model discovery, API key resolution, OAuth support.
- **`auth-storage.ts`** — auth.json for API keys and OAuth tokens.
- **`resource-loader.ts`** — skills, prompts, themes, extensions discovery.

## C++ (`ports/coding-agent/src`)

- **`AgentSession`** — loop, tools, compaction, retry, session/branch handling, destructive-bash gating, custom messages, steering/follow-up queues.
- **`agent.cpp`** — CLI, `Config` → `AgentSessionConfig`, print vs interactive.
- **`interactive_mode.cpp`** — readline loop, `/rebuild`, `/stats`, `/compact`, `/thinking`, `/queues`, `/branch`, token budget display.
- **`print_mode.cpp`** — single-prompt one-shot mode.
- **`config.cpp`** — CLI arg parsing, `settings.json` support.
- **`compaction.cpp`** — compaction logic (manual + auto).
- **`session_entry.cpp`** — JSONL session store, tree traversal, branching, summary entries.
- **`providers/llama_cpp_provider.cpp`** — llama.cpp HTTP API provider.
- **`tools/`** — built-in tools: bash, edit, find, grep, ls, read, write. Each with `ToolExecutionMode` (sequential/parallel).
- **`branch_summary.cpp`** — LLM-based branch summarization.

## Parity status (snapshot)

| Area | TS (`packages/coding-agent`) | C++ port |
|------|------------------------------|----------|
| Core turn loop + tools | Yes | Yes |
| Global parallel vs sequential | Yes (`toolExecution`) | Yes (`tool_execution_mode`) |
| Per-tool `executionMode` | Yes (`AgentTool.executionMode`) | Yes (`Tool::execution_mode()`) |
| Tool hooks (`beforeToolCall`/`afterToolCall`) | Yes (via `agent.beforeToolCall`/`afterToolCall`) | Yes (`before_tool_call`/`after_tool_call` in config) |
| `transformContext` | Yes | Yes (`transform_context` in config) |
| Dynamic API key (`getApiKey`) | Yes (via `ModelRegistry`) | Yes (`get_api_key` in config) |
| Event granularity | `tool_execution_start`/`update`/`end`, `turn_start`/`end`, `message_start`/`update`/`end` | `ToolExecutionStart`/`Update`/`End`, `ToolCall`/`ToolResult`, `TurnStart`/`TurnEnd`, `ModelCallStart` |
| Compaction (manual + auto) | Yes | Yes |
| Context overflow recovery | Yes (compact + auto-retry) | Yes (compact + retry) |
| Auto-retry (exponential backoff) | Yes | Yes |
| Session persistence (JSONL) | Yes | Yes |
| Branching (fork, branch from entry) | Yes | Yes |
| Branch summary (LLM-generated) | Yes | Yes |
| Steering/follow-up queues | Yes | Yes |
| Queue modes (all / one-at-a-time) | Yes | Yes |
| Thinking level management | Yes (off/minimal/low/medium/high/xhigh) | Yes (same levels) |
| Model cycling | Yes (scoped + all available) | Yes (cycle_model) |
| Custom messages | Yes (`sendCustomMessage`) | Yes (`sendCustomMessage`) |
| Destructive bash gating | Yes (via `tool_call` extension hook) | Yes (`destructive_bash_confirm_`) |
| Empty completion nudges | Yes | Yes |
| Session stats | Yes (`getSessionStats`) | Partial (`/stats` command, no cost/token breakdown) |
| Session export (HTML) | Yes | No |
| Session export (JSONL) | Yes (`exportToJsonl`) | Partial (session file IS JSONL, but no explicit export command) |
| Session import (JSONL) | Yes (`importFromJsonl`) | No |
| Session lifecycle (switch/new/fork) | Yes (`AgentSessionRuntime`) | Partial (`/new`, `/branch`; no runtime-level switch/fork) |
| Fork with editor text extraction | Yes | No (branching exists but no interactive fork flow) |
| Image content support | Yes (`ImageContent` in messages) | No |
| Bash execution abstraction | Yes (`BashOperations` for remote/local) | Inline (no abstraction) |
| Bash streaming output | Yes (`onChunk` callback) | Yes (via `on_chunk` in tool execution) |
| Bash abort/cancel | Yes | Yes (fork/exec + poll loop with SIGKILL process group, atomic cancel flag) |
| Bash command prefix / shell path | Yes (via settings) | No |
| Settings manager | Yes (persistent across sessions) | Partial (CLI args + `settings.json` for some options) |
| Auth storage / OAuth | Yes (`auth.json`) | No (static `api_key` in config) |
| Model registry (dynamic discovery) | Yes | No (static model string) |
| Extension system | Full plugin system | None |
| RPC mode (JSONL) | Yes | No |
| Interactive TUI (rich components) | Yes (themes, borders, loaders, selectors) | Basic readline |
| Keybindings | Yes | No |
| Slash commands (extension-based) | Yes | Partial (built-in only: /help, /stats, /compact, /thinking, /queues, /clear-queues, /new, /branch, /exit, /rebuild) |
| Prompt templates | Yes (`/template` expansion) | No |
| Skills system | Yes (`/skill:name` expansion) | No |
| Resource loader (themes, prompts, skills) | Yes | No |
| File watch | Yes | No |
| Git integration | Yes | No |
| Package manager integration | Yes | No |
| Telemetry | Yes | No |
| Image handling (resize, convert, clipboard) | Yes | No |
| Frontmatter parsing | Yes | No |
| Version checking | Yes | No |
| Config selector (multi-project) | Yes | No |
| Session picker | Yes | No |

## What's missing from C++ (core agent features)

### High priority

- **Image content** — TS messages support `ImageContent` alongside text; C++ `ChatMessage` content is plain string. The llama.cpp provider doesn't need to change (single provider), but `ChatMessage` and the session store would need to support multi-part content.
- **Bash execution abstraction** — TS `BashOperations` interface enables remote execution (e.g., via SSH). C++ bash execution is inline in the tool. Adding an abstract `BashOperations` interface would align the design.
- **Bash command prefix / shell path** — TS reads `shellCommandPrefix` and `shellPath` from settings. C++ bash tool has no such configuration.
- **Session import from JSONL** — TS `importFromJsonl()` supports importing external session files. C++ can only create new/continue recent sessions.
- **Session lifecycle management** — TS `AgentSessionRuntime` manages full session lifecycle (switch, new, fork, import) with teardown/rebind semantics. C++ has `/new` and `/branch` but no runtime-level session replacement.

### Medium priority

- **Settings manager** — TS `SettingsManager` persists model, thinking level, compaction settings, retry settings, shell config, themes across sessions. C++ has partial settings via `settings.json` but no structured manager.
- **Auth storage / OAuth** — TS `AuthStorage` + `ModelRegistry` handle API keys and OAuth tokens. C++ uses static `api_key` in config.
- **Model registry (dynamic discovery)** — TS discovers available models per provider. C++ uses a static model string.
- **Session stats (full)** — TS `getSessionStats()` returns detailed token breakdown (input/output/cache read/write), cost, tool call counts. C++ `/stats` shows basic token count.
- **Session export (HTML)** — TS `exportSessionToHtml()` renders session to styled HTML. C++ has no export.
- **Empty completion nudging** — C++ has this but TS uses it differently (TS has it in the core loop, C++ in `run_turn`). Verify parity of the nudge logic.

### Lower priority (out of scope for C++ port)

- **Extension system** — Full plugin system with `ExtensionRunner`, hooks, resource discovery. This is a TS-first feature for the TUI ecosystem.
- **RPC mode** — JSONL-based RPC for headless/remote execution.
- **Interactive TUI** — Rich TUI with themes, borders, loaders, selectors, keybindings. C++ uses basic readline.
- **Slash commands (extension-based)** — TS supports extension-registered slash commands. C++ has built-in commands only.
- **Prompt templates / Skills** — TS `/template` and `/skill:name` expansion.
- **File watch / Git / Package manager** — CLI conveniences.
- **Telemetry / Version checking** — Observability and update checks.
- **Image handling** (resize, convert, clipboard) — CLI utilities.
- **Config selector / Session picker** — Multi-project CLI UX.

## Suggested implementation order

1. **Image content** — Extend `ChatMessage` to support multi-part content (text + images).
2. **Bash execution abstraction** — Extract `BashOperations` interface, make bash tool use it.
3. **Bash command prefix / shell path** — Add to `AgentSessionConfig`, pass to bash tool.
4. **Session import** — Add `/import` command to load external JSONL files.
5. **Session lifecycle** — Add `/switch`, `/fork` commands with full session replacement semantics.
6. **Settings manager** — Structured settings persistence for model, thinking level, shell config.
7. **Session stats (full)** — Add token breakdown, cost, tool call counts to `/stats`.
8. **Model registry** — Dynamic model discovery (only relevant if multi-provider support is added later).

## Files (reference)

**Core agent:**
- `src/agent_session.{hpp,cpp}` — loop, tools, compaction, retry, branching, queues
- `src/agent.{hpp,cpp}` — CLI entry point, config → session config mapping
- `src/config.{hpp,cpp}` — CLI arg parsing, `settings.json`

**Modes:**
- `src/modes/interactive_mode.{hpp,cpp}` — readline loop, commands, `/rebuild`
- `src/modes/print_mode.{hpp,cpp}` — single-prompt mode
- `src/modes/tui_animation.{hpp,cpp}` — animation during tool rounds

**Provider:**
- `src/providers/provider.hpp` — `Provider` interface, `ChatMessage`, `ToolDefinition`, `ToolCall`
- `src/providers/llama_cpp_provider.{hpp,cpp}` — llama.cpp HTTP API

**Session store:**
- `src/session_entry.{hpp,cpp}` — JSONL session store, tree traversal, branching
- `src/compaction.{hpp,cpp}` — compaction logic

**Tools:**
- `src/tools/tool.hpp` — `Tool` base class with `execution_mode()`
- `src/tools/tool_registry.{hpp,cpp}` — registration, `build_tool_definitions`
- `src/tools/{bash,edit,find,grep,ls,read,write}_tool.{hpp,cpp}` — built-in tools
- `src/tools/bash_destructive.{hpp,cpp}` — destructive bash heuristics

**Other:**
- `src/branch_summary.{hpp,cpp}` — LLM-based branch summarization
- `src/context_loader.{hpp,cpp}` — context file loading
- `src/system_prompt.{hpp,cpp}` — system prompt generation
- `src/file_ops.{hpp,cpp}` — file operations
- `src/session_entry.cpp` — session entry serialization
- `src/pending_message_queue.hpp` — steering/follow-up queue implementation

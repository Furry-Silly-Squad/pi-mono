# C++ port vs `packages/agent` — analysis and roadmap

Primary TypeScript reference for the agent loop: **`packages/agent/src`** (`agent-loop.ts`, `Agent`, types). The C++ binary lives under **`ports/coding-agent`**.

## TypeScript (`packages/agent/src`)

- **`Agent`** — stateful wrapper: events, steering/follow-up queues, prompt/continue, abort lifecycle.
- **`agent-loop.ts`** — `runAgentLoop`, streaming, tool execution (sequential + parallel with preflight/hooks).
- **`proxy.ts`** — proxy stream for server-side LLM routing.
- **`types.ts`** — core types.

## C++ (`ports/coding-agent/src`)

- **`AgentSession`** — loop, tools, compaction, retry, session/branch handling, destructive-bash gating.
- **`agent.cpp`** — CLI, `Config` → `AgentSessionConfig`, print vs interactive.
- Tools, **`Provider`**, session store, compaction, config parsing.

## Recommendation: keep a separate program

1. **Distribution** — TS library for embedding; C++ is a standalone CLI (different product boundary).
2. **Feature mix** — C++ adds compaction, retry, sessions, branches; TS agent adds hooks, richer events, proxy, per-tool execution hints. Parity is incremental, not a line-for-line port.
3. **No shared rewrite target** — C++ is an alternative runtime, not a drop-in replacement for the TS package.
4. **Dependencies** — Separate build avoids cross-language coupling in `packages/agent`.

## Parity status (snapshot)

| Area | TS (`packages/agent`) | C++ port |
|------|-------------------------|----------|
| Core turn loop + tools | Yes | Yes |
| **Parallel tool batch** | Yes (preflight + concurrent + hooks) | **Yes** — `tool_execution_mode == "parallel"` in `AgentSession::execute_tools` (`config` / CLI `--tool-execution-mode` / `CODING_AGENT_TOOL_EXECUTION_MODE`). Emits `ToolCall` + chunks in order, runs `dispatch` on threads, appends tool results in assistant tool-call order. |
| Per-tool `executionMode` | Yes | No (global mode only) |
| `beforeToolCall` / `afterToolCall` | Yes | No |
| `transformContext` | Yes | No |
| Dynamic `getApiKey` | Yes | Static `api_key` in config |
| Proxy streaming | `streamProxy` | Not ported |
| Event granularity | Many (incl. tool execution lifecycle) | Smaller set (`AgentEvent`) |

**Parallel mode caveats (C++):** no separate preflight pass beyond bash destructive checks inside `execute_single_tool_raw`; `ToolRegistry::dispatch` is not documented thread-safe for concurrent use of the *same* tool instance; interactive destructive-bash confirmation from multiple parallel bash calls is a rough edge. TS uses sequential preflight then bounded concurrency with hooks.

## What to port next (prioritized)

### High

- [ ] **Configurable tool hooks** — `beforeToolCall` / `afterToolCall` (or `std::function` on `AgentSessionConfig`), run sequentially in preflight/finalize order compatible with parallel dispatch.
- [ ] **`transformContext`** — optional callback to trim or augment messages before each provider call.
- [ ] **Dynamic API key** — per-call resolution for OAuth / rotating tokens.
- [ ] **Steering queue modes** — confirm `QueueMode` matches TS `"all"` vs `"one-at-a-time"` behavior end-to-end.

### Medium

- [ ] **`streamProxy` equivalent** — only if the C++ CLI must route through an HTTP proxy like the TS stack.
- [ ] **Pull-based steering/follow-up** — TS callbacks vs C++ push `steer()` / `followUp()`; revisit if host apps need pull.
- [ ] **Custom message extensibility** — TS `CustomAgentMessages`; C++ `ChatMessage` is fixed unless a `type`/payload extension is added.
- [ ] **Richer events** — align with TS tool execution / message lifecycle if the TUI or tests need it.

### Lower

- [ ] **`thinkingBudgets`** — C++ has `ThinkingLevel` only.
- [ ] **Transport** — TS `sse` vs `fetch`; verify `Provider` / llama.cpp path covers needs.
- [ ] **Provider-requested retry delay cap** — align with TS `maxRetryDelayMs` behavior if not already.
- [ ] **Per-tool execution override** — mirror TS `AgentTool.executionMode` on tool definitions.

## Refactoring opportunities (C++)

- [ ] **Extract core loop** — Pull `run_turn`–equivalent logic into something like `agent_loop.{hpp,cpp}` so session composition and loop tests stay smaller (optional; parallel logic currently lives in `agent_session.cpp`).
- [ ] **Parallel execution module** — If hooks, cancellation, or a bounded thread pool land, moving parallel batching out of `AgentSession` into `parallel_tool_executor.{hpp,cpp}` may reduce complexity in `AgentSession`.
- [ ] **Event delivery** — Optional queue + callback dual path for UI-style subscribers (TS `Agent.subscribe` pattern).

## Suggested implementation order

1. ~~Parallel tool execution (global mode)~~ **done** (thread-per-call batch; config + tests).
2. **Tool hooks** — unlocks safe preflight and post-processing before tightening parallel semantics.
3. **`transformContext`** — external context control without forking `AgentSession`.
4. **Dynamic `getApiKey`** — auth parity for long sessions.
5. **Event taxonomy** — after hooks, when observability requirements are clear.
6. **Loop extraction** — when the above stabilize to avoid churn.

## Files (reference)

**Implemented parallel mode (no new top-level executor files):**

- `src/agent_session.{hpp,cpp}` — `execute_tools`, `execute_single_tool_raw`, `emit_tool_result`, `tool_dispatch_mutex_`
- `src/config.{hpp,cpp}` — `tool_execution_mode`, CLI, env, `settings.json`
- `src/agent.cpp` — maps `Config` into `AgentSessionConfig`
- `test/agent_session_test.cpp` — `parallel_tool_execution`

**Still hypothetical / future:**

- `src/agent_loop.{hpp,cpp}` — extracted loop (not created yet)
- `src/parallel_tool_executor.{hpp,cpp}` — optional split from `AgentSession`
- `src/tool_hooks.hpp` — hook types when hooks are added

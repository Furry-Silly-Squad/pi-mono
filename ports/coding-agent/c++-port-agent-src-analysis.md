# C++ port vs `packages/agent` — analysis and roadmap

Primary TypeScript reference for the agent loop: **`packages/agent/src`** (`agent-loop.ts`, `Agent`, types). Authoritative product semantics for tool execution are summarized in **`packages/agent/README.md`** (sections *With Tool Calls* and *Tools*). The C++ binary lives under **`ports/coding-agent`**.

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

## Tool execution: TypeScript semantics (`packages/agent/README.md`)

These rules govern **when the runtime may run multiple tool calls from one assistant message concurrently** and how that interacts with configuration (not something the LLM “decides” in code; the model still proposes tool calls, the runtime chooses scheduling).

| Mechanism | Behavior |
|-----------|----------|
| **Global `toolExecution`** | `"parallel"` (TS **default**) or `"sequential"`. Parallel: preflight each call in order, run allowed work concurrently, emit completion-related events as each tool finishes, but persist **`toolResult` / transcript order in assistant tool-call source order**. Sequential: one tool at a time, historical behavior. |
| **Per-tool `executionMode`** on `AgentTool` | Optional. `"parallel"` — may run concurrently with other calls in the batch. `"sequential"` — **forces the entire batch** for that assistant turn to run **sequentially**, even if global `toolExecution` is `"parallel"`. If omitted, the global setting applies to that tool. |
| **`beforeToolCall` / `afterToolCall`** | Preflight runs after args are validated; hooks can block or reshape results; `afterToolCall` can set `terminate` hints. Sequential preflight aligns extension and session state (see README *With Tool Calls*). |

**Practical effect for parity:** tools that touch shared mutable state (filesystem, DB, process env) should be marked **`executionMode: "sequential"`** in TS so they never run in parallel with siblings in the same batch. Tools that are independent can stay default-parallel. C++ today has **no** per-tool signal, so the runtime cannot downgrade a batch when e.g. `read` + `write` + `bash` are requested together under global parallel.

**Default mismatch:** TS defaults to **`toolExecution: "parallel"`**; C++ defaults to **`tool_execution_mode: "sequential"`**. Until per-tool modes exist, consider **`parallel`** as the global default in the port if the goal is closer scheduling parity with `@mariozechner/pi-agent-core` (tradeoff: higher concurrency pressure on tools that are not thread-safe).

## Parity status (snapshot)

| Area | TS (`packages/agent`) | C++ port |
|------|-------------------------|----------|
| Core turn loop + tools | Yes | Yes |
| Global parallel vs sequential | Yes (`toolExecution`) | Yes (`tool_execution_mode`, CLI / env / settings) |
| **Default** | `parallel` | **`sequential`** (differs from TS) |
| **Per-tool `executionMode`** | Yes (`AgentTool`; any `sequential` in batch → whole batch sequential) | **No** — `Tool` / `ToolDefinition` have no execution hint; `execute_tools` only looks at global flag |
| Preflight + hooks | Yes (`beforeToolCall` / `afterToolCall`) | Partial (bash destructive gating inline in `execute_single_tool_raw`; no general hooks) |
| **Parallel tool batch** | Yes — semantics above | **Yes** — when global mode is `parallel`, emits `ToolCall` + chunks in order, runs `dispatch` on threads, appends tool results in assistant tool-call order |
| `transformContext` | Yes | No |
| Dynamic `getApiKey` | Yes | Static `api_key` in config |
| Proxy streaming | `streamProxy` | Not ported |
| Event granularity | Many (incl. tool execution lifecycle) | Smaller set (`AgentEvent`) |

**Parallel mode caveats (C++):** no TS-style sequential preflight for all tools before concurrent dispatch; `ToolRegistry::dispatch` is not documented thread-safe for concurrent use of the *same* tool instance; interactive destructive-bash confirmation from multiple parallel bash calls is a rough edge.

## What to port next (prioritized)

### High

- [ ] **Per-tool `executionMode` parity** — Mirror README semantics on the C++ side so batch scheduling matches TS:
  - Extend **`Tool`** (and/or **`ToolDefinition`** if anything must be visible to the provider layer) with an optional execution hint: inherit global, **`parallel`**, or **`sequential`**.
  - In **`AgentSession::execute_tools`**: if global mode is parallel but **any** resolved tool in the batch is sequential, run the **full batch** sequentially (same “whole batch” rule as TS). If global is sequential, keep current sequential path.
  - Register built-ins with conservative defaults where needed (e.g. **`write` / `edit` / `bash`** → sequential, **`read` / `grep`** → parallel or inherit) after auditing thread safety and shared resource use.
  - Optional: align **CLI default** to `parallel` to match `packages/agent` (document in port README / help text when changed).
- [ ] **Configurable tool hooks** — `beforeToolCall` / `afterToolCall` (or `std::function` on `AgentSessionConfig`), sequential preflight order compatible with TS before launching parallel work.
- [ ] **`transformContext`** — optional callback to trim or augment messages before each provider call.
- [ ] **Dynamic API key** — per-call resolution for OAuth / rotating tokens.
- [ ] **Steering queue modes** — confirm `QueueMode` matches TS `"all"` vs `"one-at-a-time"` behavior end-to-end.

### Medium

- [ ] **`streamProxy` equivalent** — only if the C++ CLI must route through an HTTP proxy like the TS stack.
- [ ] **Pull-based steering/follow-up** — TS callbacks vs C++ push `steer()` / `followUp()`; revisit if host apps need pull.
- [ ] **Custom message extensibility** — TS `CustomAgentMessages`; C++ `ChatMessage` is fixed unless a `type`/payload extension is added.
- [ ] **Richer events** — optional alignment with `tool_execution_start` / `tool_execution_end` style lifecycle if the UI needs completion-order signals while keeping persisted order.

### Lower

- [ ] **`thinkingBudgets`** — C++ has `ThinkingLevel` only.
- [ ] **Transport** — TS `sse` vs `fetch`; verify `Provider` / llama.cpp path covers needs.
- [ ] **Provider-requested retry delay cap** — align with TS `maxRetryDelayMs` behavior if not already.

## Refactoring opportunities (C++)

- [ ] **Extract core loop** — Pull `run_turn`–equivalent logic into something like `agent_loop.{hpp,cpp}` so session composition and loop tests stay smaller (optional; parallel logic currently lives in `agent_session.cpp`).
- [ ] **Parallel execution module** — If hooks, per-tool modes, cancellation, or a bounded thread pool land, moving batch planning + concurrent dispatch into `parallel_tool_executor.{hpp,cpp}` may shrink `AgentSession`.
- [ ] **Event delivery** — Optional queue + callback dual path for UI-style subscribers (TS `Agent.subscribe` pattern).

## Suggested implementation order

1. ~~Parallel tool execution (global only)~~ **done** (thread-per-call batch; config + tests).
2. **Per-tool `executionMode` + batch rule** — matches TS scheduling and makes global `parallel` safe for mixed tool batches.
3. **Tool hooks** — sequential preflight and post-processing aligned with TS.
4. **`transformContext`** — external context control without forking `AgentSession`.
5. **Dynamic `getApiKey`** — auth parity for long sessions.
6. **Event taxonomy** — after hooks, when observability requirements are clear.
7. **Loop extraction** — when the above stabilize to avoid churn.

## Files (reference)

**Implemented global parallel mode:**

- `src/agent_session.{hpp,cpp}` — `execute_tools`, `execute_single_tool_raw`, `emit_tool_result`, `tool_dispatch_mutex_`
- `src/config.{hpp,cpp}` — `tool_execution_mode`, CLI, env, `settings.json`
- `src/agent.cpp` — maps `Config` into `AgentSessionConfig`
- `src/providers/provider.hpp` — `ToolDefinition` (name, description, schema only today)
- `src/tools/tool.hpp`, `tool_registry.cpp` — registration and `build_tool_definitions`
- `test/agent_session_test.cpp` — `parallel_tool_execution`

**Likely touch points for `executionMode` parity:**

- `src/tools/tool.hpp` — optional virtual or enum field for execution mode
- `src/providers/provider.hpp` — only if the mode must be exposed on definitions sent to the model (TS keeps it on `AgentTool`, not necessarily in the LLM tool list; usually **runtime-only** on the C++ `Tool` + registry lookup in `execute_tools`)
- `src/agent_session.cpp` — batch classification: any sequential tool → sequential path; else honor global parallel

**Still hypothetical / future:**

- `src/agent_loop.{hpp,cpp}` — extracted loop (not created yet)
- `src/parallel_tool_executor.{hpp,cpp}` — optional split from `AgentSession`
- `src/tool_hooks.hpp` — hook types when hooks are added

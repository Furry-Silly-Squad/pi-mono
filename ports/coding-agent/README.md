# coding-agent C++ Port

A standalone C++ CLI port of the TypeScript `packages/coding-agent`. Implements a tool-calling agent loop with session management, context compaction, and interactive TUI.

## Scope

| Area | Status |
|------|--------|
| Build | Linux + macOS, CMake, C++20 |
| Provider | `llama-cpp` (OpenAI-compatible `POST /v1/chat/completions`) |
| Agent Loop | `AgentSession` class with full turn loop |
| Tools | `read`, `write`, `edit`, `bash`, `grep`, `find`, `ls` |
| Parallel Tool Execution | Yes (global mode; sequential defaults for write/edit/bash) |
| Tool Hooks | `before_tool_call` / `after_tool_call` callbacks |
| Context Transform | `transform_context` callback |
| Session Model | Full tree model with branches, labels, 10 entry types |
| Branch Summaries | LLM-generated handoff between sessions |
| Context Compaction | Iterative boundaries, valid cut points, split-turn summaries, file-op tracking |
| Interactive Mode | Readline REPL with TUI animation, token budget display |
| Token Feedback | Per-turn breakdown, `/stats`, `/tokens`, compaction proximity |
| Interrupt | Ctrl+C cancels LLM requests, TUI spinner |
| Queue Management | `steer()` / `followUp()` with `OneAtATime` / `All` modes |
| Auto-Retry | Exponential backoff for rate limits, 5xx, timeouts |
| Print Mode | One-shot `--prompt` mode |
| Settings | CLI flags, env vars, `~/.config/coding-agent/settings.json` |
| Tests | 10 CTest targets (offline, no LLM required) |

## Build

Prerequisites:

- CMake 3.20+
- C++20 compiler (`clang++` or `g++`)
- libcurl development package
- readline development package

Install dependencies:

- macOS (Homebrew): `brew install cmake curl readline pkg-config`
- Ubuntu/Debian: `sudo apt-get install -y cmake build-essential libcurl4-openssl-dev libreadline-dev pkg-config`

Build:

```bash
cmake -S ports/coding-agent -B ports/coding-agent/build
cmake --build ports/coding-agent/build
```

Binary:

`ports/coding-agent/build/coding-agent`

### Tests (local, no LLM)

Offline checks live under `ports/coding-agent/test/` and are registered with CTest:

```bash
cmake -S ports/coding-agent -B ports/coding-agent/build
cmake --build ports/coding-agent/build -j
ctest --test-dir ports/coding-agent/build --output-on-failure
```

| Target | What it checks |
|--------|----------------|
| `coding-agent-fileops-test` | `extract_file_ops_from_messages`, `merge_file_ops`, footer formatting |
| `coding-agent-branch-summary-test` | Branch summary generation from fixture JSONL |
| `coding-agent-session-store-test` | SessionManager roundtrip: append, reload, tree traversal |
| `coding-agent-branch-traversal-test` | JSONL tree (`parent_id`): `get_branch`, `find_common_ancestor`, `collect_entries_for_branch_summary` |
| `coding-agent-compaction-carry-forward-test` | Fake provider + `compact_history()` merges prior `FileOps` with summarized window |
| `coding-agent-edit-tool-test` | `EditTool::execute()` atomic write via temp file |
| `coding-agent-bash-destructive-test` | `bash_command_looks_destructive()` detection |
| `coding-agent-agent-session-test` | Full `AgentSession` integration (tools, compaction, events) |
| `coding-agent-agent-session-abort-test` | Abort/cancel flag handling in agent loop |
| `coding-agent-phase9-test` | Queue management (`steer`/`followUp`), retry logic, custom messages |

If CMake cannot find readline on macOS/Homebrew:

```bash
export PKG_CONFIG_PATH="/opt/homebrew/opt/readline/lib/pkgconfig:$PKG_CONFIG_PATH"
cmake -S ports/coding-agent -B ports/coding-agent/build
```

If you previously configured with missing dependencies, wipe the build dir and reconfigure:

```bash
rm -rf ports/coding-agent/build
cmake -S ports/coding-agent -B ports/coding-agent/build
cmake --build ports/coding-agent/build
```

## Run

Start llama.cpp server separately, then:

```bash
ports/coding-agent/build/coding-agent \
  --provider llama-cpp \
  --base-url http://127.0.0.1:8080 \
  --prompt "Explain this repo in one paragraph."
```

### CLI Flags

| Flag | Description |
|------|-------------|
| `--provider <id>` | Provider name (default `llama-cpp`) |
| `--base-url <url>` | Provider base URL (default `http://127.0.0.1:8080`) |
| `--model <id>` | Model ID |
| `--api-key <key>` | API key |
| `--max-tokens <int>` | Max output tokens (`--n-predict` alias) |
| `--temperature <float>` | Sampling temperature |
| `--session <id>` | Resume session by ID |
| `--new-session` | Start a new session (with branch handoff from latest) |
| `--no-branch-summary` | Skip LLM branch handoff when starting new/resuming session |
| `--print` | Print mode (one-shot, no REPL) |
| `--no-tools` | Disable all tools |
| `--no-context-files` | Skip `.context` file loading |
| `--context-size <int>` | Context window size in tokens |
| `--compaction-reserve-tokens <int>` | Reserve tokens before compaction (default `16384`) |
| `--compaction-keep-recent-tokens <int>` | Keep recent tokens after compaction (default `20000`) |
| `--no-compaction-fail-fast` | Skip compaction on failure instead of erroring |
| `--active-tools <comma-sep>` | Restrict active tools (e.g., `read,bash`) |
| `--tool-execution-mode <mode>` | `sequential` or `parallel` (default `sequential`) |
| `--cwd <dir>` | Working directory |
| `--no-stream` | Disable streaming |
| `--retry-enabled` | Enable auto-retry (default `true`) |
| `--no-retry-enabled` | Disable auto-retry |
| `--retry-max-retries <int>` | Max retry attempts (default `3`) |
| `--retry-base-delay-ms <int>` | Base delay in ms (default `1000`) |
| `--retry-max-delay-ms <int>` | Max delay cap in ms (default `60000`) |

### Environment Variables

- `CODING_AGENT_PROVIDER` (default `llama-cpp`)
- `CODING_AGENT_BASE_URL` (default `http://127.0.0.1:8080`)
- `CODING_AGENT_MODEL` (default empty)
- `CODING_AGENT_API_KEY` (default empty)

### Settings File

Optional: `~/.config/coding-agent/settings.json`

Supported fields: `base_url`, `model`, `api_key`, `temperature`, `max_tokens`, `context_size`, `compaction_reserve_tokens`, `compaction_keep_recent_tokens`, `compaction_fail_fast`, `retry_enabled`, `retry_max_retries`, `retry_base_delay_ms`, `retry_max_retry_delay_ms`, `tool_execution_mode`, `initial_active_tools`.

## Interactive Mode Commands

When running in interactive mode (no `--print`), the following slash commands are available:

| Command | Description |
|---------|-------------|
| `/compact` | Manually trigger context compaction |
| `/stats` | Full session/token breakdown (ID, message count, token usage, compaction history) |
| `/tokens` | Abbreviated token count only |
| `/thinking [level]` | Set or cycle thinking level (`off`, `low`, `medium`, `high`, `xhigh`) |
| `/clear` | No-op (documented) |
| `/exit` | Exit the REPL |
| `/new` | Start a new session with branch handoff |
| `/branch` | Record a branch point in the current session |
| `/queues` | List queued steering/follow-up messages |
| `/clear-queues` | Clear all message queues |

## Architecture

### Core Classes

| Class | Responsibility |
|-------|---------------|
| `AgentSession` | Full agent runtime: turn loop, tool execution, compaction, retry, queue management, events |
| `SessionManager` | Tree-model session persistence: 10 entry types, branching, labels, migration, `buildSessionContext()` |
| `PendingMessageQueue` | Mode-aware message queue (`OneAtATime` / `All` drain strategy) |
| `ToolRegistry` | Tool registration and dispatch (parallel or sequential) |
| `LlamaCppProvider` | HTTP provider via libcurl (streaming, SSE, cancel via `cancel_flag`) |

### Session JSONL Entry Types

Session logs are stored under `~/.config/coding-agent/sessions/*.jsonl`.

| Type | Description |
|------|-------------|
| `session` | Header row: version, id, timestamp, cwd |
| `message` | User, assistant, or tool result messages |
| `compaction` | Compaction event: summary, token counts, first_kept_entry_id, file ops |
| `branch_summary` | Branch handoff: summary, source_session_id, handoff_source_leaf_id, file ops |
| `label` | Label assignment to an entry |
| `custom` | Arbitrary custom entry with JSON data |
| `custom_message` | Custom message with content and display text |
| `session_info` | Session metadata (name) |
| `thinking_level_change` | Thinking level change event |
| `model_change` | Model change event |
| `compaction_skipped` | Compaction skipped due to failure |

Every entry has `id`, `parentId`, and `timestamp`. Tree linkage via `parent_id` (root → leaf traversal).

### Compaction

- Triggers when estimated context tokens exceed `context_size - compaction_reserve_tokens`
- Keeps a recent tail (`compaction_keep_recent_tokens`), summarizes older history
- Valid cut points: never cuts inside tool-result blocks; handles split-turns with prefix summaries
- File operations (`read_files`, `modified_files`) are carried forward across compaction windows
- Failure policy: configurable via `--no-compaction-fail-fast` (skip vs error)

### Branch Summaries

On `--new-session` or resuming a different session via `--session <id>`, the latest session file is summarized:

1. Walk from leaf toward root (or to common ancestor if `target_id` specified)
2. Apply token budget, collect entries
3. Generate LLM summary with structured format (Goal, Constraints, Progress, Key Decisions, Next Steps)
4. Append file-op footer
5. Inject as synthetic context on reload, except when `handoff_source_leaf_id` appears on the current leaf path (prevents double-injection)

### Queue Management

- `steer(text)` — queued messages delivered before the next LLM call
- `followUp(text)` — queued messages delivered when the agent stops (no tool calls)
- Modes: `OneAtATime` (drain first item only) or `All` (drain all items)
- Queue state is in-memory; not persisted to session files

### Auto-Retry

- Detects retryable errors: rate limits (429), overloaded (503), server errors (500), timeouts, connection errors
- Exponential backoff: `base_delay * 2^(attempt-1)`, capped at `max_retry_delay_ms`
- Interruptible via cancel flag
- Emits `auto_retry_start` / `auto_retry_end` events

## Documentation

See `docs/` for implementation details:

- [docs/index.md](docs/index.md) — Documentation index
- [docs/c++-port-agent-src-analysis.md](docs/c++-port-agent-src-analysis.md) — TS vs C++ parity analysis
- [docs/issues.md](docs/issues.md) — Known issues and fix directions
- [docs/phases/](docs/phases/) — Phase implementation plans (1–16)

## Troubleshooting

- **Compaction fails**: Check provider connectivity/model health; run exits with `Compaction failed: ...`. Use `--no-compaction-fail-fast` to skip instead.
- **Compaction triggers too early**: Raise `--context-size` or lower `--compaction-reserve-tokens`.
- **Too much history summarized**: Increase `--compaction-keep-recent-tokens`.
- **Readline not found on macOS**: `export PKG_CONFIG_PATH="/opt/homebrew/opt/readline/lib/pkgconfig:$PKG_CONFIG_PATH"` then reconfigure.
- **Build errors after dependency changes**: `rm -rf ports/coding-agent/build` and reconfigure.

# coding-agent C++ port (v0)

This directory contains a first stripped-down C++ port of `packages/coding-agent`.

Scope for v0:

- Linux + macOS CLI build
- Only `llama-cpp` provider
- Tool-calling agent loop (`read`, `write`, `edit`, `bash`, `grep`, `find`, `ls`)
- Print mode and readline interactive mode
- Linear JSONL session persistence with auto-resume
- Context compaction when token budget grows too large

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
| `coding-agent-branch-summary-test` | Fixture JSONL and `summarize_branch_session_file()` |
| `coding-agent-session-store-test` | Temp-dir session file: hydrate compaction `read_files`/`modified_files`, append, reload |
| `coding-agent-compaction-carry-forward-test` | Fake provider + `compact_history()` merges prior `FileOps` with summarized window |

`SessionStore` accepts an optional session directory (second constructor argument) so tests never write under `~/.config`; production code uses the default path only.

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

Optional flags:

- `--model <id>`
- `--api-key <key>`
- `--max-tokens <int>` (`--n-predict` alias)
- `--temperature <float>`
- `--session <id>`
- `--new-session`
- `--print`
- `--no-tools`
- `--no-context-files`
- `--context-size <int>`
- `--compaction-reserve-tokens <int>` (default `16384`)
- `--compaction-keep-recent-tokens <int>` (default `20000`)
- `--cwd <dir>`
- `--no-stream`

Environment variables:

- `CODING_AGENT_PROVIDER` (default `llama-cpp`)
- `CODING_AGENT_BASE_URL` (default `http://127.0.0.1:8080`)
- `CODING_AGENT_MODEL` (default empty)
- `CODING_AGENT_API_KEY` (default empty)

Settings file (optional):

- `~/.config/coding-agent/settings.json`
- Supported fields: `base_url`, `model`, `api_key`, `temperature`, `max_tokens`, `context_size`, `compaction_reserve_tokens`, `compaction_keep_recent_tokens`

## Notes

- Provider integration targets OpenAI-compatible `POST /v1/chat/completions`.
- Tool-call chunks are parsed from both non-stream and SSE stream responses.
- `nlohmann/json` is fetched automatically during CMake configure via `FetchContent`.
- The `bash` tool prompts for confirmation before running commands that look destructive (for example `rm`, `git reset --hard`, or `git clean -fd`).
- Compaction triggers when estimated context tokens are above `context_size - compaction_reserve_tokens`.
- Compaction keeps a recent tail (`compaction_keep_recent_tokens`) and summarizes only older history.
- Compaction failures are surfaced as runtime errors (not silently ignored).
- Interactive/print output includes compaction events like `[compaction] 12345 -> 6789 tokens`.
- Starting with `--new-session` stores a compact branch handoff summary from the latest prior session.

## Session JSONL rows

Session logs are stored under `~/.config/coding-agent/sessions/*.jsonl`.

In addition to `session` and `message` rows, the port now persists:

- `compaction` rows:
  - `tokens_before`
  - `tokens_after`
  - `first_kept_index`
  - `first_kept_entry_id` (when present)
  - `summary`
  - `read_files`, `modified_files` (arrays of paths)
- `branch_summary` rows:
  - `source_session_id`
  - `summary`
  - `read_files`, `modified_files` (arrays of paths)

On resume, these rows are reloaded as synthetic assistant context messages.

## Troubleshooting

- If compaction fails, check provider connectivity/model health first; the run now exits with `Compaction failed: ...`.
- If compaction triggers too early, raise `--context-size` or lower `--compaction-reserve-tokens`.
- If too much history is summarized away, increase `--compaction-keep-recent-tokens`.

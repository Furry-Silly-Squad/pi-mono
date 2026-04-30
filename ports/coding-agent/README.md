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
- `--cwd <dir>`
- `--no-stream`

Environment variables:

- `CODING_AGENT_PROVIDER` (default `llama-cpp`)
- `CODING_AGENT_BASE_URL` (default `http://127.0.0.1:8080`)
- `CODING_AGENT_MODEL` (default empty)
- `CODING_AGENT_API_KEY` (default empty)

Settings file (optional):

- `~/.config/coding-agent/settings.json`
- Supported fields: `base_url`, `model`, `api_key`, `temperature`, `max_tokens`, `context_size`

## Notes

- Provider integration targets OpenAI-compatible `POST /v1/chat/completions`.
- Tool-call chunks are parsed from both non-stream and SSE stream responses.
- `nlohmann/json` is fetched automatically during CMake configure via `FetchContent`.

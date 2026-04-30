# coding-agent C++ port (v0)

This directory contains a first stripped-down C++ port of `packages/coding-agent`.

Scope for v0:

- Linux + macOS CLI build
- Only `llama-cpp` provider
- Single prompt-in / single response-out flow
- No tools, no session persistence, no TUI/RPC modes

## Build

Prerequisites:

- CMake 3.20+
- C++20 compiler (`clang++` or `g++`)
- libcurl development package

Build:

```bash
cmake -S ports/coding-agent -B ports/coding-agent/build
cmake --build ports/coding-agent/build
```

Binary:

`ports/coding-agent/build/coding-agent`

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
- `--n-predict <int>`
- `--temperature <float>`
- `--no-stream`

Environment variables:

- `CODING_AGENT_PROVIDER` (default `llama-cpp`)
- `CODING_AGENT_BASE_URL` (default `http://127.0.0.1:8080`)
- `CODING_AGENT_MODEL` (default empty)
- `CODING_AGENT_API_KEY` (default empty)

## Notes

- The current HTTP integration targets OpenAI-compatible `POST /v1/chat/completions`.
- Streaming mode parses Server-Sent Events (`data: ...`) and prints token deltas as they arrive.

# PHASE-16: Empty Completion Nudges

## Goal

Document and validate the empty completion nudge behavior currently implemented in the C++ port, identify any parity gaps with the TypeScript implementation, and specify any adjustments needed for alignment.

## TypeScript Reference

The TS `AgentSession` (`packages/coding-agent/src/core/agent-session.ts`) does **not** implement empty completion nudging inline. Instead:

- The TS `AgentSession` delegates to `pi-agent-core`'s `Agent` class for the core turn loop.
- The `Agent` class in `pi-agent-core` handles the `agent_end` event and checks for empty completions via the extension system.
- Extensions (notably the TUI extension) can intercept `agent_end` and inject a nudge when the assistant returns no content and no tool calls.
- The nudge message text and behavior are extension-specific, not hardcoded in `AgentSession`.

The `AgentSession` config option `max_empty_completion_nudges` exists in the TS port but is consumed by the TUI extension layer, not by `AgentSession` itself.

## Current C++ State

The C++ port implements empty completion nudging inline in `AgentSession::run_turn()` (`agent_session.cpp`, lines 666-682):

```cpp
// Empty content: nudge or check follow-up
if (config_.max_empty_completion_nudges > 0 &&
    empty_completion_nudges < config_.max_empty_completion_nudges) {
    ++empty_completion_nudges;
    ChatMessage nudge{
        .role    = "user",
        .content = kEmptyCompletionUserNudge,
    };
    nudge.entry_id = session_->appendMessage(nudge);
    messages_.push_back(nudge);
    on_chunk("\n[empty assistant message; retrying with nudge…]\n");
    continue;
}
```

**Behavior:**

1. After a model call, if `response.tool_calls.empty()` AND `response.content` is whitespace/empty:
   - Check if `config_.max_empty_completion_nudges > 0` and `empty_completion_nudges < config_.max_empty_completion_nudges`.
   - If so, append a synthetic user nudge message to the session and message history.
   - Emit the nudge text via `on_chunk` callback.
   - Continue the turn loop (another model call).
2. If nudges are exhausted (count reached max), fall through to follow-up queue check, then break.
3. The nudge message is persisted to the session file (JSONL) as a regular user message.
4. The nudge counter is per-turn (local to `run_turn()`), reset on each new user prompt.
5. `TurnDebugInfo.empty_completion_nudges` tracks the count for diagnostics.

**Nudge text:**

```cpp
const char* kEmptyCompletionUserNudge =
    "Your last reply had no message text. Briefly summarize what you did, what you found, and "
    "what the user should do next. If more tool calls are required, use them. Do not reply with an "
    "empty message.";
```

**Config sources:**

- `config.max_empty_completion_nudges` — default 2, 0 = off.
- CLI flag: `--max-empty-nudges <n>`.
- Env var: `CODING_AGENT_MAX_EMPTY_NUDGES`.
- `settings.json` key: `max_empty_completion_nudges`.

## Parity Analysis

| Aspect | TS | C++ | Notes |
|---|---|---|---|
| Nudge logic location | Extension layer (`pi-agent-core` + TUI extension) | Inline in `AgentSession::run_turn()` | Different architecture; C++ has no extension system |
| Nudge message text | Extension-specific (TUI provides default) | Hardcoded `kEmptyCompletionUserNudge` | C++ text is reasonable default |
| Max nudges per turn | Configurable via `max_empty_completion_nudges` | Configurable via `max_empty_completion_nudges` | Same |
| Nudge persisted to session | Yes (as synthetic user message) | Yes (appended to session) | Same |
| Nudge counter per-turn | Yes | Yes | Same |
| Follow-up queue check after nudges exhausted | Yes | Yes | Same |
| Chunk callback output | Extension-specific | `[empty assistant message; retrying with nudge…]` | C++ output is clear |
| Turn debug tracking | Yes | Yes (`TurnDebugInfo.empty_completion_nudges`) | Same |
| Nudge only on empty+no-tools | Yes | Yes | Same |
| Nudge skips if follow-up queued | Yes | Yes | Same |

**Key difference:** The TS implementation is more flexible — the nudge text and behavior are configurable via extensions. The C++ implementation is hardcoded but sufficient for the single-provider, no-extension design.

## Design Assessment

The current C++ implementation is **correct and complete** for the C++ port's architecture. No changes are needed because:

1. **No extension system** — without extensions, inline nudge logic is the right approach.
2. **Single provider** — the nudge text is a reasonable default for all llama.cpp-backed models.
3. **Session persistence** — nudges are properly persisted to the session JSONL.
4. **Configurable** — `max_empty_completion_nudges` can be tuned via CLI, env, or settings.
5. **Diagnostics** — `TurnDebugInfo.empty_completion_nudges` provides visibility.

## Potential Enhancements (Future)

If the C++ port later adds extension support or a pluggable nudge system:

1. **Configurable nudge text** — allow the nudge message to be set via settings or CLI (`--nudge-text`).
2. **Nudge via settings** — move `max_empty_completion_nudges` into `SettingsManager` (PHASE-15).
3. **Nudge extension hook** — allow extensions to provide custom nudge text or disable nudging entirely.

## Testing

- **Unit test**: Model returns empty content, verify nudge is appended and turn continues.
- **Unit test**: Model returns empty content with 0 nudges allowed, verify turn ends.
- **Unit test**: Model returns empty content with nudges exhausted, verify follow-up queue is checked.
- **Unit test**: Model returns non-empty content, verify no nudge is sent.
- **Unit test**: Model returns tool calls with empty content, verify no nudge is sent (tool calls take priority).
- **Integration test**: Session with nudges — verify nudge messages are persisted to JSONL.
- **Integration test**: Config override — `--max-empty-nudges 0` disables nudging.

## Dependencies

- No new dependencies.
- No file changes required — the current implementation is correct.

## Implementation Order

1. **No code changes** — the C++ implementation matches the required behavior.
2. **Add unit tests** — verify nudge logic in isolation.
3. **Add integration tests** — verify session persistence of nudge messages.
4. **Document** — add entry to `docs/c++-port-agent-src-analysis.md` parity table noting the architecture difference (inline vs extension-layer) but behavioral parity.

## Conclusion

The C++ port's empty completion nudge implementation is functionally correct and architecturally appropriate for a no-extension design. The only action needed is adding tests and documenting the architectural difference with TS (inline vs extension-layer).

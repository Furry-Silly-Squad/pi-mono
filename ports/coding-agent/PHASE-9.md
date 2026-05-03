# Phase 9: Queue Management + Retry with Exponential Backoff

## Goal

Add message queueing (steer/followUp) and auto-retry with exponential backoff to the C++ `AgentSession`. These features allow users to queue messages while the agent is streaming (interrupt or wait), and automatically retry transient LLM errors (rate limits, overloaded, 5xx) with backoff.

## Complexity: Medium

## Prerequisites

- Phase 8 complete: `SessionManager` tree model wired into `AgentSession`.
- Build passes with zero errors and zero new warnings before starting.
- `AgentSession::run_turn()` is the single entry point for LLM calls (no `agent_loop` free function).

---

## Current State of the Port (as of Phase 8)

### Core infrastructure (implemented)

| Area | Status |
|------|--------|
| `AgentSession` class with `run()` / `run_turn()` | Done (Phase 7) |
| `SessionManager` tree model with 10 entry types | Done (Phase 8) |
| Tools: bash, edit, find, grep, ls, read, write | Done |
| LlamaCpp provider (streaming, SSE, cancel) | Done |
| Auto-compaction (threshold + manual) | Done |
| Branch summarization | Done |
| Interactive mode (readline REPL) | Done |
| Print mode | Done |
| Event system (single handler) | Done |

### What is missing (this phase)

| Area | Notes |
|------|-------|
| **Steer queue** | Messages queued while streaming, injected after current turn |
| **Follow-up queue** | Messages queued while streaming, injected only when agent would stop |
| **Queue modes** | `one-at-a-time` vs `all` drain strategies |
| **Auto-retry** | Exponential backoff on retryable errors (rate limit, overloaded, 5xx) |
| **Retry settings** | Configurable `enabled`, `maxRetries`, `baseDelayMs` |
| **Queue events** | `queue_update` event for UI display |
| **`waitForRetry()`** | Blocks callers until retry completes |
| **`streamingBehavior` option** | `steer` vs `followUp` when queuing during streaming |
| **Pending next-turn messages** | "Asides" injected alongside the next user prompt |
| **`sendCustomMessage()` with delivery modes** | `steer`, `followUp`, `nextTurn` |

---

## Phase 9 Scope

### 1. New types and enums (`agent_session.hpp`)

- [ ] `QueueMode` — `"all"` or `"one-at-a-time"` drain strategy.
- [ ] `StreamingBehavior` — `"steer"` or `"followUp"`.
- [ ] `RetrySettings` — `enabled: bool`, `maxRetries: int`, `baseDelayMs: int`.
- [ ] `ProviderRetrySettings` — `timeoutMs: int`, `maxRetries: int`, `maxRetryDelayMs: int`.
- [ ] `SteeringMessage` — text + optional images for queued steering.
- [ ] `FollowUpMessage` — text + optional images for queued follow-up.
- [ ] `PendingNextTurnMessage` — custom message with delivery metadata.
- [ ] `QueueUpdateEvent` — `type: "queue_update"`, `steering: vector<string>`, `followUp: vector<string>`.
- [ ] `AutoRetryStartEvent` — `type: "auto_retry_start"`, `attempt`, `maxAttempts`, `delayMs`, `errorMessage`.
- [ ] `AutoRetryEndEvent` — `type: "auto_retry_end"`, `success`, `attempt`, `finalError`.
- [ ] Extended `CompactionEndEvent` — adds `willRetry: bool`, `errorMessage: string`.

### 2. Message queue classes

Implement `PendingMessageQueue` as a simple queue with mode-aware drain:

```cpp
class PendingMessageQueue {
public:
    explicit PendingMessageQueue(QueueMode mode);
    void enqueue(std::string text, std::vector<ImageContent> images = {});
    bool has_items() const;
    void clear();
    // drain() returns messages respecting mode:
    //   one-at-a-time: returns only the first item, leaves rest
    //   all: returns all items, clears queue
    std::vector<std::pair<std::string, std::vector<ImageContent>>> drain();
    QueueMode mode() const;
    void set_mode(QueueMode mode);
private:
    QueueMode mode_;
    std::vector<std::pair<std::string, std::vector<ImageContent>>> items_;
};
```

### 3. `AgentSession` queue state members

Add to `AgentSession` private state:

```cpp
// Queue state
PendingMessageQueue steering_queue_;
PendingMessageQueue follow_up_queue_;

// Tracking for UI display (mirrors queue content)
std::vector<std::string> steering_messages_;
std::vector<std::string> follow_up_messages_;

// Pending next-turn messages (asides)
std::vector<CustomMessage> pending_next_turn_messages_;

// Retry state
std::unique_ptr<RetrySettings> retry_settings_;
int retry_attempt_ = 0;
bool retry_enabled_ = false;

// For waitForRetry()
std::atomic<bool> retry_in_progress_{false};
std::condition_variable retry_cv_;
std::mutex retry_mutex_;
```

### 4. Queue API on `AgentSession`

Add public methods:

```cpp
// Queue a steering message while the agent is running.
// Delivered after the current assistant turn finishes its tool calls,
// before the next LLM call.
void steer(const std::string& text, const std::vector<ImageContent>& images = {});

// Queue a follow-up message.
// Delivered only when the agent has no more tool calls and no steering messages.
void followUp(const std::string& text, const std::vector<ImageContent>& images = {});

// Clear queues.
void clear_steering_queue();
void clear_follow_up_queue();
void clear_all_queues();

// Check if either queue has pending messages.
bool has_queued_messages() const;

// Get current queue modes.
QueueMode steering_mode() const;
void set_steering_mode(QueueMode mode);
QueueMode follow_up_mode() const;
void set_follow_up_mode(QueueMode mode);
```

### 5. Queue event emission

```cpp
private void emit_queue_update();
```

This emits a `queue_update` event with the current `steering_messages_` and `follow_up_messages_` vectors. Called after any queue mutation (enqueue, drain, clear).

### 6. Auto-retry logic

#### 6a. `_is_retryable_error(const ChatMessage& msg) -> bool`

Determine if an assistant error message is retryable. Retryable errors:

- `stopReason == "error"` AND the error text contains:
  - `"rate limit"` / `"rateLimit"` / `"429"` (HTTP rate limit)
  - `"overloaded"` / `"503"` / `"service unavailable"` (LLM overloaded)
  - `"500"` / `"internal error"` (server error)
  - `"timeout"` (request timeout)
  - `"connection reset"` / `"connection refused"` (network error)

Non-retryable errors (return false):
- Tool execution errors
- User input errors
- Authentication errors
- Context overflow (handled separately by compaction)

#### 6b. `_handle_retryable_error(const ChatMessage& msg) -> bool`

If the error is retryable and retry is enabled, initiate the retry loop:

1. Increment `retry_attempt_`.
2. Calculate backoff delay: `delay_ms = base_delay_ms * (2 ^ (retry_attempt_ - 1))`.
3. Cap delay at `max_retry_delay_ms` from `ProviderRetrySettings` (default 60000ms).
4. Emit `auto_retry_start` event.
5. Sleep for `delay_ms` (interruptible by cancel flag).
6. If cancelled: emit `auto_retry_end{success: false, finalError: "cancelled"}`, return false.
7. Call `call_provider()` again with the same messages.
8. On success: reset `retry_attempt_`, emit `auto_retry_end{success: true}`, return true.
9. On another retryable error: loop back to step 4 (up to `maxRetries`).
10. If max retries exceeded: emit `auto_retry_end{success: false, finalError: last_error}`, return false.

Return `true` if retry was initiated (caller should skip compaction and continue). Return `false` if retry was not applicable.

#### 6c. `waitForRetry() -> void`

Block the calling thread until retry completes:

1. If `!retry_in_progress_`, return immediately.
2. Lock `retry_mutex_`, wait on `retry_cv_` until `retry_in_progress_` is false.
3. Unlock and return.

Called by `prompt()` after `agent.prompt()` resolves (i.e., after `run_turn()` returns).

#### 6d. `_resolve_retry()`

Signal the retry condition variable and clear state:

1. `retry_in_progress_ = false;`
2. `retry_cv_.notify_all();`

Called at the end of `_handle_retryable_error()` (success or failure) and when a non-error response arrives.

#### 6e. `_create_retry_state_for_agent_end(ChatMessage& msg)`

Called at the start of `_process_agent_event` when `event.type == "agent_end"`:

1. If `!retry_enabled_`, return.
2. If `retry_in_progress_`, return (already retrying).
3. If `msg.stopReason != "error"`, clear retry state and return.
4. If `_is_retryable_error(msg)`: set `retry_in_progress_ = true`, initialize `_retryPromise`.

### 7. Integrate queues into `run_turn()`

The current `run_turn()` loop in `agent_session.cpp` calls `call_provider()` once per iteration, then checks for tool calls. Update to:

1. **Before `call_provider()`**: Drain `steering_queue_` and prepend queued steering messages to the message list. These are injected as user messages before the LLM call.

2. **After tool execution, before next `call_provider()`**: Drain `steering_queue_` again and prepend any remaining steering messages.

3. **When `run_turn()` completes with no tool calls** (agent would stop): Drain `follow_up_queue_` and prepend follow-up messages. Loop back to `call_provider()` if there are follow-up messages.

4. **When `run_turn()` completes successfully**: If `retry_attempt_ > 0` (from a previous turn), emit `auto_retry_end{success: true}` and reset `retry_attempt_ = 0`.

5. **On agent_end with error**: Call `_handle_retryable_error()`. If it returns true (retry initiated), do NOT proceed to compaction — return early. If it returns false (no retry / max retries), clear retry state and proceed to compaction.

Pseudocode for the updated `run_turn()`:

```cpp
bool AgentSession::run_turn() {
    // Append user message...
    emit_event(TurnStart);

    for (int i = 0; i < max_tool_iterations_; i++) {
        emit_event(ModelCallStart);

        // Prepend pending steering messages before LLM call
        auto steering_msgs = steering_queue_.drain();
        for (auto& [text, images] : steering_msgs) {
            messages_.push_back(make_user_message(text, images));
        }

        auto result = call_provider();

        // Append assistant message...
        emit_event(ToolResult) for each tool call...

        check_and_compact();

        if (no tool calls) {
            // Agent would stop. Check follow-up queue.
            auto followup_msgs = follow_up_queue_.drain();
            if (!followup_msgs.empty()) {
                for (auto& [text, images] : followup_msgs) {
                    messages_.push_back(make_user_message(text, images));
                }
                continue; // Continue the turn with follow-up messages
            }
            break; // No more messages, turn ends
        }
    }

    // Reset retry state on successful completion
    if (retry_attempt_ > 0) {
        emit_event(AutoRetryEnd{success: true, attempt: retry_attempt_});
        retry_attempt_ = 0;
    }

    emit_event(TurnEnd);
    return true;
}
```

### 8. Integrate retry into event processing

In the event handler that processes `agent_end` (currently in `agent_session.cpp`):

```cpp
// After agent completes, check for retryable errors
if (event.type == "agent_end" && last_assistant_message) {
    if (_is_retryable_error(last_assistant_message)) {
        bool did_retry = _handle_retryable_error(last_assistant_message);
        if (did_retry) return; // Skip compaction, retry was initiated
    }
    _resolve_retry();
    check_compaction(last_assistant_message);
}
```

### 9. Add `streamingBehavior` to config / prompt options

Add `streaming_behavior` field to `AgentSessionConfig`:

```cpp
struct PromptOptions {
    bool expand_prompt_templates = true;
    std::vector<ImageContent> images;
    StreamingBehavior streaming_behavior = StreamingBehavior::Steer; // steer or followUp
    // ...
};
```

When `prompt()` is called while `running_` is true, queue via `steer()` or `followUp()` based on this option.

### 10. Add `sendCustomMessage()` with delivery modes

```cpp
enum class CustomMessageDelivery { Steer, FollowUp, NextTurn };

void send_custom_message(const std::string& custom_type,
                         const std::string& content,
                         const std::string& display,
                         const std::vector<nlohmann::json>& details = {},
                         CustomMessageDelivery delivery = CustomMessageDelivery::NextTurn);
```

- `Steer`: Enqueue to steering queue with custom message type.
- `FollowUp`: Enqueue to follow-up queue with custom message type.
- `NextTurn`: Append to `pending_next_turn_messages_`, delivered alongside next user prompt.

### 11. Retry settings integration

#### 11a. Settings in `AgentSessionConfig`

Add to `AgentSessionConfig`:

```cpp
struct AgentSessionConfig {
    // ... existing fields ...

    // Retry settings
    bool retry_enabled = true;
    int retry_max_retries = 3;
    int retry_base_delay_ms = 1000;
    int retry_max_retry_delay_ms = 60000;
    int retry_timeout_ms = 30000;
};
```

#### 11b. Settings persistence (optional, future)

The TypeScript `SettingsManager` persists retry settings to JSON. For Phase 9, retry settings are passed via `AgentSessionConfig` from `main.cpp` / `agent.cpp`. Future phase can wire these to a settings file.

### 12. Wire into `main.cpp` and interactive mode

- `agent.cpp` reads retry settings from config / CLI flags and passes to `AgentSessionConfig`.
- Interactive mode displays queue status (current steering/follow-up messages) in the prompt area.
- `/queues` or similar command to list / clear queued messages.
- Display retry progress: `[retry #2/3] Waiting 4s before retry...`

### 13. Build clean with `-Werror`

- [ ] Zero errors, zero new warnings.
- [ ] All existing tests pass.
- [ ] New tests for queue and retry pass.

---

## Tasks

### 1. New types (`agent_session.hpp`)

- [ ] `QueueMode` enum class: `OneAtATime`, `All`.
- [ ] `StreamingBehavior` enum class: `Steer`, `FollowUp`.
- [ ] `RetrySettings` struct: `enabled`, `maxRetries`, `baseDelayMs`.
- [ ] `ProviderRetrySettings` struct: `timeoutMs`, `maxRetries`, `maxRetryDelayMs`.
- [ ] `QueueUpdateEvent` variant in `AgentEvent`.
- [ ] `AutoRetryStartEvent` variant in `AgentEvent`.
- [ ] `AutoRetryEndEvent` variant in `AgentEvent`.
- [ ] Extended `CompactionEndEvent` with `willRetry` and `errorMessage`.

### 2. `PendingMessageQueue` class (`pending_message_queue.hpp/cpp`)

- [ ] Constructor with `QueueMode`.
- [ ] `enqueue(text, images)`.
- [ ] `has_items()`.
- [ ] `clear()`.
- [ ] `drain()` — mode-aware extraction.
- [ ] `mode()` / `set_mode()`.

### 3. Queue state in `AgentSession` (`agent_session.hpp`)

- [ ] `steering_queue_` / `follow_up_queue_` members.
- [ ] `steering_messages_` / `follow_up_messages_` tracking vectors.
- [ ] `pending_next_turn_messages_` vector.
- [ ] Retry state members: `_retry_settings`, `_retry_attempt`, `_retry_in_progress`, `_retry_cv`, `_retry_mutex`.

### 4. Queue API methods (`agent_session.cpp`)

- [ ] `steer(text, images)`.
- [ ] `followUp(text, images)`.
- [ ] `clear_steering_queue()`, `clear_follow_up_queue()`, `clear_all_queues()`.
- [ ] `has_queued_messages()`.
- [ ] `steering_mode()` / `set_steering_mode()`.
- [ ] `follow_up_mode()` / `set_follow_up_mode()`.

### 5. Queue event emission (`agent_session.cpp`)

- [ ] `emit_queue_update()` — emits `queue_update` event.
- [ ] Called from `steer()`, `followUp()`, `clear_*_queue()`, and on drain in `run_turn()`.

### 6. Auto-retry: `_is_retryable_error()` (`agent_session.cpp`)

- [ ] Detect rate limit, overloaded, 5xx, timeout, connection errors in assistant message.

### 7. Auto-retry: `_handle_retryable_error()` (`agent_session.cpp`)

- [ ] Retry loop with exponential backoff.
- [ ] Backoff cap at `maxRetryDelayMs`.
- [ ] Cancel support via `cancel_flag_`.
- [ ] Emit `auto_retry_start` / `auto_retry_end`.
- [ ] Return bool indicating if retry was initiated.

### 8. Auto-retry: `waitForRetry()` / `_resolve_retry()` (`agent_session.cpp`)

- [ ] `waitForRetry()` — blocks on condition variable.
- [ ] `_resolve_retry()` — signals condition variable.

### 9. Integrate queues into `run_turn()` (`agent_session.cpp`)

- [ ] Drain `steering_queue_` before each `call_provider()`.
- [ ] Drain `follow_up_queue_` when agent would stop (no tool calls).
- [ ] Reset `retry_attempt_` on successful completion.

### 10. Integrate retry into event processing (`agent_session.cpp`)

- [ ] Check `_is_retryable_error()` on agent_end.
- [ ] Call `_handle_retryable_error()`, skip compaction if retry initiated.
- [ ] Call `_resolve_retry()` on non-retryable completion.

### 11. `streamingBehavior` in `prompt()` (`agent_session.cpp`)

- [ ] When `running_` is true, use `streaming_behavior` to choose steer vs followUp.

### 12. `sendCustomMessage()` (`agent_session.cpp`)

- [ ] Implement with `CustomMessageDelivery` enum.
- [ ] Steer/FollowUp: enqueue to respective queue.
- [ ] NextTurn: append to `pending_next_turn_messages_`, delivered in next `prompt()`.

### 13. Retry settings in `AgentSessionConfig` and `main.cpp`

- [ ] Add fields to `AgentSessionConfig`.
- [ ] Wire from `Config` in `agent.cpp`.
- [ ] Add CLI flags: `--retry-enabled`, `--retry-max-retries`, `--retry-base-delay-ms`.

### 14. Interactive mode: queue display and commands

- [ ] Display current queue messages in prompt area.
- [ ] `/queues` command: list queued messages.
- [ ] `/clear-queues` command: clear all queues.

### 15. Retry progress display

- [ ] Display `[retry #2/3] Waiting 4s...` during retry wait.
- [ ] Display `[retry] Success after 2 attempts` on successful retry.
- [ ] Display `[retry] Failed after 3 attempts` on max retries.

### 16. Tests

Create test file `test/agent_session_queue_retry_test.cpp`:

- [ ] `queue_steer_drain_before_call` — steering messages are drained before next LLM call.
- [ ] `queue_followup_drain_after_stop` — follow-up messages are drained only when agent stops.
- [ ] `queue_mode_one_at_a_time` — drain returns only first message.
- [ ] `queue_mode_all` — drain returns all messages.
- [ ] `is_retryable_error_rate_limit` — "429 rate limit" is retryable.
- [ ] `is_retryable_error_overloaded` — "overloaded" is retryable.
- [ ] `is_retryable_error_500` — "500" is retryable.
- [ ] `is_retryable_error_timeout` — "timeout" is retryable.
- [ ] `is_retryable_error_tool_fail` — tool execution error is NOT retryable.
- [ ] `is_retryable_error_auth` — auth error is NOT retryable.
- [ ] `retry_exponential_backoff` — delays double each attempt.
- [ ] `retry_max_delay_cap` — delay capped at `maxRetryDelayMs`.
- [ ] `retry_cancel` — retry is interrupted by cancel flag.
- [ ] `retry_max_retries_exceeded` — stops after `maxRetries` attempts.
- [ ] `retry_success_resets_counter` — retry_attempt resets on success.
- [ ] `prompt_during_streaming_queues_by_behavior` — steer vs followUp based on option.
- [ ] `send_custom_message_steer` — custom message queued to steer queue.
- [ ] `send_custom_message_next_turn` — custom message delivered with next prompt.

---

## Validation

```bash
# Build
cmake -S ports/coding-agent -B ports/coding-agent/build
cmake --build ports/coding-agent/build -j

# Run tests
cd ports/coding-agent/build && ctest --output-on-failure

# Smoke test: steer during streaming
ports/coding-agent/build/coding-agent --base-url http://127.0.0.1:8080
# → Type a long prompt, quickly type another message (should steer)

# Smoke test: followUp during streaming
ports/coding-agent/build/coding-agent --base-url http://127.0.0.1:8080
# → Type a prompt, wait for agent to stop, type another message

# Smoke test: queue display
# → Type messages while agent is running, see them listed in prompt area

# Smoke test: retry (simulate rate limit)
# → Use a test provider that returns 429 on first call, verify auto-retry

# Verify /queues and /clear-queues commands work
```

## Acceptance Criteria

- [ ] `PendingMessageQueue` class implemented with `OneAtATime` and `All` modes.
- [ ] `steer()` and `followUp()` queue messages correctly.
- [ ] Queue messages are drained at the correct points in `run_turn()` (steer before LLM call, followUp when agent stops).
- [ ] `queue_update` event emitted on all queue mutations.
- [ ] `steering_mode()` / `follow_up_mode()` get/set work.
- [ ] `_is_retryable_error()` correctly identifies rate limit, overloaded, 5xx, timeout, connection errors.
- [ ] `_is_retryable_error()` correctly rejects tool errors, auth errors, context overflow.
- [ ] `_handle_retryable_error()` implements exponential backoff with configurable settings.
- [ ] Backoff delay is capped at `maxRetryDelayMs`.
- [ ] `waitForRetry()` blocks until retry completes.
- [ ] Cancel flag interrupts retry wait.
- [ ] Max retries respected — stops after N attempts.
- [ ] `auto_retry_start` / `auto_retry_end` events emitted correctly.
- [ ] Successful retry resets `retry_attempt_` to 0.
- [ ] Retry skips compaction when initiated (returns early).
- [ ] `streamingBehavior` option routes queued prompts to steer vs followUp.
- [ ] `sendCustomMessage()` with `deliverAs` options works correctly.
- [ ] `pending_next_turn_messages_` delivered alongside next user prompt.
- [ ] `clear_all_queues()` clears both queues.
- [ ] `has_queued_messages()` returns correct status.
- [ ] Interactive mode displays queue messages and retry progress.
- [ ] `/queues` command lists queued messages.
- [ ] `/clear-queues` command clears all queues.
- [ ] Build clean with `-Werror`.
- [ ] All 6 existing tests pass.
- [ ] All new queue/retry tests pass.

---

## Migration Notes

### New Events

The following `AgentEvent` variants are added:

- `queue_update`: Emitted when queue state changes. Carries current `steering` and `followUp` message lists.
- `auto_retry_start`: Emitted before each retry attempt. Carries `attempt`, `maxAttempts`, `delayMs`, `errorMessage`.
- `auto_retry_end`: Emitted when retry completes. Carries `success`, `attempt`, optional `finalError`.

### Existing Event Changes

- `compaction_end`: Adds optional `willRetry` field (true if retry was triggered after compaction failure) and `errorMessage` (error message if compaction failed and retry was attempted).

### Config Changes

- `AgentSessionConfig` gains: `retry_enabled`, `retry_max_retries`, `retry_base_delay_ms`, `retry_max_retry_delay_ms`, `retry_timeout_ms`.
- `PromptOptions` gains: `streaming_behavior` (default `Steer`).

### Session File Compatibility

Queue state and retry state are ephemeral (in-memory only). No session file changes required.

---

## Deferred to Later Phases

These features are related but out of scope for Phase 9:

| Feature | Reason |
|---------|--------|
| Retry settings persistence | Settings manager not yet wired into C++ port |
| Per-provider retry settings | Requires full provider integration |
| CustomMessage with image content | Image support deferred |
| `/tree` / fork / clone commands | SessionManager tree UI deferred |
| Extension command queuing | Extension system deferred |
| RPC mode queue support | RPC mode deferred |
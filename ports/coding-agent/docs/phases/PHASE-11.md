# Phase 11: Image Content Support

## Overview

Add multi-part message content (text + images) to the C++ agent, matching the TypeScript `AgentSession` model where `content` is `string | (TextContent | ImageContent)[]`. Images are base64-encoded with MIME type metadata, stored inline in JSONL session files.

This phase covers the core data model, session persistence, provider serialization, and agent-loop plumbing. Image viewing (display in TUI) is deferred to a future GUI/web UI phase.

## Scope

### In scope
- Extend `ChatMessage` to support multi-part content (text + images)
- Extend `AgentEvent` to carry image references in tool results
- Update `SessionManager` to serialize/deserialize image content in JSONL
- Update `LlamaCppProvider` to serialize multi-part content to HTTP API payloads
- Update `AgentSession` run loop to pass images through in user messages, steering, follow-up, and custom messages
- Update `BranchSummary` to handle image content in branch entries
- Update `Compaction` to preserve images in compacted context
- Update all built-in tools to optionally return image results (e.g., `read` tool with image files)
- Update `PendingMessageQueue` to carry images (already has the field)

### Out of scope (deferred)
- Image resizing/compression (TS has `autoResizeImages` from settings)
- Image viewing in TUI (requires GUI/web UI)
- Clipboard image paste support
- Image file attachment from CLI
- Custom messages with images (support in data model, but no CLI entry point)
- Tool results with images (support in data model, `read` tool can return images)

## Design Decisions

### 1. Content model

Replace `ChatMessage.content: std::string` with a variant:

```cpp
enum class ContentType { Text, Image };

struct MessageContentPart {
    ContentType type;
    std::string text;       // populated when type == Text
    std::string data;       // base64 image data (populated when type == Image)
    std::string mime_type;  // e.g., "image/jpeg", "image/png" (populated when type == Image)
};

struct ChatMessage {
    std::string role;
    std::variant<std::string, std::vector<MessageContentPart>> content;
    // ... existing fields
};
```

This matches the TS model exactly: `string | (TextContent | ImageContent)[]`.

**Rationale**: A variant allows both legacy single-string messages and new multi-part messages. Serialization handles both transparently.

### 2. Image serialization

Images stored inline in JSONL as base64:

```json
{"type": "message", "id": "...", "parentId": "...", "timestamp": "...",
 "message": {"role": "user", "content": [{"type":"text","text":"look at this"},
                                          {"type":"image","data":"base64...","mimeType":"image/png"}]}}
```

**Rationale**: Inline storage is simplest. Session files grow with images, but images are typically small (screenshots < 1MB). Sidecar files add filesystem complexity without clear benefit for a CLI tool.

### 3. Provider abstraction

Extend `provider.hpp` with `MessageContentPart` type. The `Provider` interface and `ChatRequest`/`ChatResponse` structs remain unchanged at the top level — the provider implementation handles serialization of multi-part content to the HTTP API.

**Rationale**: Even with a single provider, a generic abstraction makes future multi-provider work cleaner. The llama.cpp provider will serialize multi-part content to the llama.cpp HTTP API format.

### 4. Image viewing

**Deferred to a future GUI/web UI phase.** The data model supports images end-to-end, but there is no display mechanism in the current readline-based interactive mode.

**Why defer:**
- The current TUI is readline-based (single-line input, line-buffered output). It has no concept of rendering images.
- A proper image display requires either:
  - A **GUI toolkit** (e.g., Dear ImGui, Qt) — adds native UI dependency
  - A **web UI** (embedded browser or separate web server) — adds HTTP server dependency
  - **Terminal-based image display** (e.g., `kitty` graphics protocol, `sixel`, `ueberzug`) — limited terminal support, fragile
- None of these approaches are trivial and each has different trade-offs.
- The user has expressed interest in eventually connecting C++ to an app (GUI or web UI).

**Recommendation**: Build the image data path first (this phase), then decide on the display layer based on the eventual GUI/web UI architecture. When the display layer is ready, it can consume `MessageContentPart` from the message history.

### 5. Image resizing

**Deferred.** TS has `settingsManager.getImageAutoResize()` that resizes images before sending to the LLM. C++ has no settings manager yet. Add this when settings persistence is implemented (Phase 8+).

## Implementation Plan

### Step 1: Core types

**Files**: `src/providers/provider.hpp`

- Add `MessageContentPart` struct with `type`, `text`, `data`, `mimeType`
- Add `ContentType` enum
- Change `ChatMessage.content` from `std::string` to `std::variant<std::string, std::vector<MessageContentPart>>`
- Add helper functions:
  - `is_text_only(const ChatMessage&)` — returns true if content is a plain string or single text part
  - `extract_text(const ChatMessage&)` — extracts all text parts concatenated
  - `get_images(const ChatMessage&)` — returns vector of image parts

### Step 2: Session serialization

**Files**: `src/session_entry.hpp`, `src/session_entry.cpp`

- Update `SessionMessageEntry` serialization/deserialization to handle multi-part content
- JSON format:
  - Legacy: `"content": "plain string"`
  - New: `"content": [{"type":"text","text":"..."},{"type":"image","data":"...","mimeType":"..."}]`
- Deserialization must handle both formats (backward compat)
- Add `nlohmann::json` to/from `MessageContentPart` conversion

### Step 3: Agent event extension

**Files**: `src/agent_session.hpp`

- Extend `AgentEvent` to carry image data in `ToolResult` events:
  - Add `std::vector<MessageContentPart> tool_result_images` field
- Update `ToolExecutionEnd` event to include images

### Step 4: Queue updates

**Files**: `src/pending_message_queue.hpp`

- Already has `std::vector<std::string> images` per message. Change to `std::vector<MessageContentPart>`.
- Update `enqueue()` and `drain()` signatures.

### Step 5: AgentSession run loop

**Files**: `src/agent_session.cpp`, `src/agent_session.hpp`

- Update `run_turn()` to handle images in user input
- Update `steer()` and `followUp()` to carry `std::vector<MessageContentPart>` images
- Update `sendCustomMessage()` to accept image content
- Update `emit_tool_result()` to include images from tool results
- Update `check_and_compact()` to preserve images through compaction
- Update `handle_retryable_error()` to preserve images through retry

### Step 6: Provider serialization

**Files**: `src/providers/llama_cpp_provider.hpp`, `src/providers/llama_cpp_provider.cpp`

- Update payload builder to serialize multi-part content
- For llama.cpp HTTP API, images are sent as base64 data URIs in the message content array
- Handle `MessageContentPart` conversion to llama.cpp request JSON format

### Step 7: Tool updates

**Files**: `src/tools/read_tool.cpp`

- `read` tool: when reading an image file, return the image as a `MessageContentPart` instead of text
- Add detection for image MIME types (`.png`, `.jpg`, `.jpeg`, `.gif`, `.webp`, `.bmp`)
- Read image file as binary, base64-encode, return as image content part

### Step 8: Branch summary & compaction

**Files**: `src/branch_summary.cpp`, `src/compaction.cpp`, `src/compaction.hpp`

- `compact_history()`: preserve images from messages that are kept (not compacted away)
- `branchWithSummary()`: handle image content in branch entries
- Summary generation: text-only summaries (images are not included in summary text, but image entries are preserved in the kept message list)

### Step 9: Context building

**Files**: `src/session_entry.cpp` (buildSessionContext)

- `SessionManager::buildSessionContext()` must handle multi-part content when building the message list for the LLM
- Extract text from mixed content for context token counting
- Preserve image parts in the context

### Step 10: Testing

**Files**: `test/agent_session_test.cpp`

- Add tests for:
  - Multi-part message serialization/deserialization
  - Image content in steering/follow-up queues
  - Image content in tool results (read tool)
  - Image preservation through compaction
  - Branch summary with image content
  - Provider payload serialization with images

## GUI/Web UI Considerations

### Current state

The C++ agent has a readline-based interactive mode. It has no mechanism for displaying images. The TypeScript version uses a rich TUI with components that render images inline.

### Options for image display

#### Option A: Terminal-based image display

Use terminal graphics protocols:
- **kitty graphics protocol** — modern, works in kitty terminal
- **sixel** — works in xterm, iTerm2, some Linux terminals
- **ueberzug++** — overlays images in terminal (works across terminals)
- **iterm2 inline images** — iTerm2-specific

Pros: No new dependencies, works in terminal
Cons: Limited terminal support, fragile, small display area

#### Option B: Embedded GUI (Dear ImGui)

Add Dear ImGui as a dependency, render the TUI with image support.

Pros: Native look, good image rendering, cross-platform
Cons: Adds significant dependency, requires window management, conflicts with readline

#### Option C: Embedded web UI

Add a lightweight HTTP server (e.g., cpp-httplib) and serve a web UI. Images rendered in browser.

Pros: Full image support, rich UI, cross-platform, familiar web tech
Cons: Adds HTTP dependency, browser window, more complex architecture

#### Option D: External app connection

Connect C++ agent to an external app (mobile app, desktop app) via a protocol (WebSocket, local HTTP).

Pros: Decoupled, app can handle images natively
Cons: Requires external app, more complex setup

### Recommendation

**Defer the display decision.** Phase 11 should focus on getting the data model correct end-to-end. When the GUI/web UI layer is chosen:

1. The data model (`MessageContentPart`) is already in place
2. The session store already serializes images
3. The provider already sends images to the LLM
4. The display layer just needs to consume `ChatMessage` content and render images

The display layer should integrate with the existing event system (`AgentEventHandler`), receiving messages with image content and rendering them appropriately.

## Files to modify

| File | Change |
|------|--------|
| `src/providers/provider.hpp` | Add `MessageContentPart`, update `ChatMessage.content` |
| `src/session_entry.hpp` | Update serialization helpers |
| `src/session_entry.cpp` | Multi-part JSON serialization/deserialization |
| `src/agent_session.hpp` | Update `steer()`/`followUp()` signatures, `AgentEvent` fields |
| `src/agent_session.cpp` | Run loop image handling, tool result images |
| `src/pending_message_queue.hpp` | Update `images` field type |
| `src/providers/llama_cpp_provider.cpp` | Multi-part payload serialization |
| `src/tools/read_tool.cpp` | Return images for image files |
| `src/compaction.cpp` | Preserve images through compaction |
| `src/branch_summary.cpp` | Handle image content in branches |
| `test/agent_session_test.cpp` | New tests for image content |

## Parity status (post-implementation)

| Area | TS | C++ (after Phase 11) |
|------|----|---------------------|
| Image content in messages | Yes | Yes (data model + persistence) |
| Image content in steering/follow-up | Yes | Yes |
| Image content in tool results | Yes | Yes (read tool) |
| Image content in custom messages | Yes | Partial (data model, no CLI entry) |
| Image resizing | Yes | No (deferred) |
| Image viewing/display | Yes (TUI) | No (deferred to GUI/web UI) |
| Image file attachment from CLI | No | No |
| Clipboard image paste | Yes | No |

# Phase 7: Missing Functionality (TypeScript -> C++ Port)

This file catalogs functionality present in `packages/coding-agent/src` that is not yet implemented in `ports/coding-agent/src`.

---

## 1. Agent Session (agent-session.ts)

The C++ port now includes an `AgentSession` API declaration in `agent_session.hpp`, but runtime execution is still wired through `agent_loop.cpp` from `agent.cpp`.

| Feature | TypeScript | C++ Port |
|---------|-----------|----------|
| AgentSession class | Full lifecycle, state, events | Partial (declared in `agent_session.hpp`, not wired into runtime path) |
| Event subscription system (subscribe, _handleAgentEvent, _emit) | Yes | Partial (event types + handler API declared, no implementation wired) |
| Agent state (state, model, thinkingLevel, isStreaming, systemPrompt) | Yes | Partial (state fields declared, runtime still uses `run_agent_loop`) |
| Model management (setModel, cycleModel, cycleThinkingLevel, setThinkingLevel) | Yes | Partial (API declared, no implementation wired) |
| Thinking levels (off, minimal, low, medium, high, xhigh) | Yes | Partial (`off..high` declared; no `xhigh` level in C++ enum) |
| Queue management (steer, followUp, clearQueue) | Yes | No |
| Auto-compaction (overflow recovery, threshold-based) | Yes | Partial (threshold-triggered in `agent_loop`; no `AgentSession` integration) |
| Branch summarization (navigateTree, generateBranchSummary) | Yes | Partial (branch_summary.cpp exists but not wired to session) |
| Auto-retry with exponential backoff | Yes | No |
| Bash execution with streaming and abort | Yes | No (bash is a tool, not a session-level operation) |
| Session name management | Yes | No |
| Tree navigation (navigateTree with summarization) | Yes | No |
| Session stats (getSessionStats) | Yes | No |
| HTML export (exportToHtml) | Yes | No |
| JSONL export (exportToJsonl) | Yes | No |
| Custom messages (sendCustomMessage) | Yes | No |
| User messages with images (sendUserMessage) | Yes | No |
| Prompt template expansion | Yes | No |
| Skill command expansion (/skill:name) | Yes | No |
| Extension system integration (ExtensionRunner, bindExtensions, reload) | Yes | No |
| Tool registry management (getActiveToolNames, setActiveToolsByName, getAllTools) | Yes | Partial (API declared on `AgentSession`) |
| Context usage tracking (getContextUsage) | Yes | No |
| Model cycling with scoped models | Yes | No |

---

## 2. Session Manager (session-manager.ts)

The C++ `SessionStore` remains a linear JSONL store (not a tree manager), but it now persists and reloads several non-message event row types.

| Feature | TypeScript | C++ Port |
|---------|-----------|----------|
| SessionManager class | Full tree traversal, branching | No (SessionStore is flat) |
| Session entry types (message, compaction, custom, branchSummary, bashExecution, modelChange, thinkingLevelChange, sessionInfo, label) | 9+ entry types | Partial (`message`, `compaction`, `branch_summary`, `compaction_skipped`) |
| Branch/leaf management (branch, resetLeaf, getLeafId) | Yes | No |
| Custom entries (compactionSummary, branchSummary, bashExecution, modelChange, thinkingLevelChange, sessionInfo, label) | Yes | Partial (`compaction` + `branch_summary` rows supported) |
| Labels on entries | Yes | No |
| Session migration (migrateSessionEntries) | Yes | No |
| Session context building (buildSessionContext) | Yes | No |
| Version tracking (CURRENT_SESSION_VERSION) | Yes | No |

---

## 3. Auth Storage (auth-storage.ts)

| Feature | TypeScript | C++ Port |
|---------|-----------|----------|
| AuthStorage class | API key + OAuth credentials | No |
| FileAuthStorageBackend | Yes | No |
| InMemoryAuthStorageBackend | Yes | No |
| OAuth credential management | Yes | No |
| Auth status tracking | Yes | No |

---

## 4. Model Registry (model-registry.ts)

| Feature | TypeScript | C++ Port |
|---------|-----------|----------|
| ModelRegistry class | API key resolution, model discovery | No |
| Provider registration/unregistration | Yes | No |
| OAuth detection | Yes | No |

---

## 5. Settings Manager (settings-manager.ts)

| Feature | TypeScript | C++ Port |
|---------|-----------|----------|
| SettingsManager class | Full settings persistence | No (no dedicated manager class) |
| Compaction settings (enabled, reserveTokens, keepRecentTokens) | Yes | Partial (loaded via settings/env/CLI in `config.cpp`) |
| Retry settings (enabled, maxRetries, baseDelayMs) | Yes | No |
| Image settings (autoResize) | Yes | No |
| Shell settings (commandPrefix, shellPath) | Yes | No |
| Default model/provider persistence | Yes | Partial (`~/.config/coding-agent/settings.json` + env support) |
| Default thinking level persistence | Yes | No |
| Steering/follow-up mode persistence | Yes | No |
| Theme persistence | Yes | No |
| Settings reload | Yes | No |

---

## 6. Compaction (core/compaction/)

| Feature | TypeScript | C++ Port |
|---------|-----------|----------|
| compact() | Full compaction with cut-point finding | Partial (compact_history in compaction.cpp) |
| prepareCompaction | Session entry preparation | No |
| findCutPoint | Smart cut-point algorithm | No |
| calculateContextTokens | Token counting | Partial (total_context_tokens) |
| estimateContextTokens | Estimate from usage data | No |
| shouldCompact | Threshold checking | Partial (should_compact) |
| generateBranchSummary | Branch summarization | Partial (branch_summary.cpp) |
| collectEntriesForBranchSummary | Entry collection | Partial |
| serializeConversation | Session serialization | Partial |
| Compaction settings (from SettingsManager) | Yes | Partial (from `Config` loaded via settings/env/CLI) |
| Auto-compaction (overflow + threshold) | Yes | Partial (threshold check + compaction in `agent_loop`) |
| Compaction failure handling (fail-fast vs graceful skip) | Yes | Partial (`compaction_fail_fast` and graceful skip mode both supported) |

---

## 7. Extension System (core/extensions/)

| Feature | TypeScript | C++ Port |
|---------|-----------|----------|
| ExtensionRunner | Full extension runtime | No |
| ExtensionContext | Session control methods | No |
| ExtensionAPI | sendMessage, setModel, compact, etc. | No |
| Extension loading and discovery | Yes | No |
| Extension hooks (tool_call, tool_result, session_before_compact, session_before_tree, input, resources_discover, etc.) | Yes | No |
| Extension commands (slash commands) | Yes | No |
| Extension UI components (widgets, dialogs, selectors) | Yes | No |
| Extension flag values | Yes | No |
| wrapRegisteredTools | Tool wrapping for extensions | No |
| defineTool | Tool definition helper | No |
| discoverAndLoadExtensions | Extension discovery | No |

---

## 8. Interactive Mode Components (modes/interactive/components/)

The C++ port has a readline-based `interactive_mode.cpp` with command handling, token budget/status output, Ctrl+C cancellation, and TUI animation, but not the TypeScript component architecture.

| Component | TypeScript | C++ Port |
|-----------|-----------|----------|
| ArminComponent | ASCII art animation | No |
| AssistantMessageComponent | Message display | No |
| BashExecutionComponent | Bash output display | No |
| BorderedLoader | Loading animation | No |
| BranchSummaryMessageComponent | Branch summary display | No |
| CompactionSummaryMessageComponent | Compaction summary display | No |
| CustomEditor | Custom file editor | No |
| CustomMessageComponent | Custom message display | No |
| Daxnuts | ASCII art animation | No |
| DynamicBorder | Dynamic border rendering | No |
| ExtensionEditorComponent | Extension file editor | No |
| ExtensionInputComponent | Extension input | No |
| ExtensionSelectorComponent | Extension selector | No |
| FooterComponent | Status bar | No |
| KeybindingHints | Keybinding hints | No |
| LoginDialogComponent | OAuth/API key login | No |
| ModelSelectorComponent | Model selection | No |
| OAuthSelectorComponent | OAuth provider selection | No |
| SessionSelectorComponent | Session selection | No |
| SettingsSelectorComponent | Settings editor | No |
| ShowImagesSelectorComponent | Image display toggle | No |
| SkillInvocationMessageComponent | Skill invocation display | No |
| ThemeSelectorComponent | Theme selection | No |
| ThinkingSelectorComponent | Thinking level selector | No |
| ToolExecutionComponent | Tool execution display | No |
| TreeSelectorComponent | Session tree navigation | No |
| UserMessageComponent | User message display | No |
| UserMessageSelectorComponent | User message selection | No |
| VisualTruncate | Visual line truncation | No |
| keyHint / keyText | Key formatting utilities | No |
| renderDiff | Diff rendering | No |
| truncateToVisualLines | Visual line truncation | No |

---

## 9. Theme System (modes/interactive/theme/)

| Feature | TypeScript | C++ Port |
|---------|-----------|----------|
| Theme class | Full theme with colors | No |
| ThemeColor | Color definitions | No |
| getLanguageFromPath | Language detection | No |
| getMarkdownTheme | Markdown theme | No |
| getSelectListTheme | List theme | No |
| getSettingsListTheme | Settings theme | No |
| highlightCode | Code highlighting | No |
| initTheme | Theme initialization | No |

---

## 10. RPC Mode (modes/rpc/)

| Feature | TypeScript | C++ Port |
|---------|-----------|----------|
| runRpcMode | RPC server mode | No |
| RpcClient | RPC client | No |
| JSONL protocol | Yes | No |
| RpcCommand / RpcResponse / RpcSessionState | RPC types | No |

---

## 11. Print Mode (modes/print-mode.ts)

| Feature | TypeScript | C++ Port |
|---------|-----------|----------|
| runPrintMode | Print mode implementation | Partial (`print_mode.cpp` supports prompt/stdin one-shot flow via `run_agent_loop`) |

---

## 12. Bash Executor (core/bash-executor.ts)

| Feature | TypeScript | C++ Port |
|---------|-----------|----------|
| executeBashWithOperations | Streaming bash execution | No (bash is a tool, not session-level) |
| BashSpawnContext | Spawn context | No |
| BashSpawnHook | Spawn hooks | No |
| BashOperations interface | Bash operations abstraction | No |
| createLocalBashOperations | Local bash operations | No |

---

## 13. Tool System (core/tools/)

| Feature | TypeScript | C++ Port |
|---------|-----------|----------|
| withFileMutationQueue | File mutation queue | No |
| Edit diff operations (edit-diff.ts) | Yes | No |
| File mutation queue (file-mutation-queue.ts) | Yes | No |
| Tool definition wrapper (tool-definition-wrapper.ts) | Yes | No |
| Render utilities (render-utils.ts) | Yes | No |
| Path utilities (path-utils.ts) | Yes | No |
| Truncation (truncate.ts) | Yes | No |
| Bash tool options (commandPrefix, shellPath, width) | Yes | No |
| Edit tool options (autoResizeImages) | Yes | No |
| Grep tool options (maxResults, contextLines) | Yes | No |
| Find tool options | Yes | No |
| Ls tool options | Yes | No |
| Read tool options (maxLines, maxBytes) | Yes | Partial |
| Write tool options | Yes | No |
| DEFAULT_MAX_BYTES / DEFAULT_MAX_LINES | Yes | No |
| formatSize | Size formatting | No |
| truncateHead / truncateTail / truncateLine | Truncation utilities | No |

---

## 14. CLI (cli/)

| Feature | TypeScript | C++ Port |
|---------|-----------|----------|
| args.ts | Comprehensive CLI argument parsing | Partial (config.cpp has basic args) |
| config-selector.ts | Config file selector | No |
| file-processor.ts | File processing | No |
| initial-message.ts | Initial message handling | No |
| list-models.ts | Model listing | No |
| session-picker.ts | Session picker | No |

---

## 15. Core Utilities (core/)

| Feature | TypeScript | C++ Port |
|---------|-----------|----------|
| event-bus.ts | Event bus | No |
| diagnostics.ts | Diagnostics | No |
| exec.ts | Exec utilities | No |
| export-html/ | HTML export (ansi-to-html, tool-renderer) | No |
| footer-data-provider.ts | Footer data provider | No |
| keybindings.ts | Keybindings management | No |
| messages.ts | Message types (BashExecutionMessage, CustomMessage) | No |
| output-guard.ts | Output guard | No |
| package-manager.ts | Package manager | No |
| prompt-templates.ts | Prompt template expansion | No |
| provider-display-names.ts | Provider display names | No |
| resolve-config-value.ts | Config value resolution | No |
| resource-loader.ts | Resource loading (skills, prompts, themes, context files, system prompt) | Partial (context_loader.cpp) |
| session-cwd.ts | Session CWD management | No |
| skills.ts | Skills loading | No |
| slash-commands.ts | Slash commands | No |
| source-info.ts | Source info | No |
| system-prompt.ts | System prompt building | Partial (system_prompt.cpp) |
| timings.ts | Timing utilities | No |
| defaults.ts | Default values | No |
| migrations.ts | Session migrations | No |
| auth-guidance.ts | Auth guidance messages | No |

---

## 16. SDK (core/sdk.ts)

| Feature | TypeScript | C++ Port |
|---------|-----------|----------|
| createAgentSession | SDK session factory | No |
| createAgentSessionRuntime | SDK runtime factory | No |
| createAgentSessionServices | SDK services factory | No |
| createBashTool | Bash tool factory | No |
| createCodingTools | Coding tools factory | No |
| createEditTool | Edit tool factory | No |
| createFindTool | Find tool factory | No |
| createGrepTool | Grep tool factory | No |
| createLsTool | Ls tool factory | No |
| createReadOnlyTools | Read-only tools factory | No |
| createReadTool | Read tool factory | No |
| createWriteTool | Write tool factory | No |
| PromptTemplate | Prompt template type | No |

---

## 17. Utilities (utils/)

| Feature | TypeScript | C++ Port |
|---------|-----------|----------|
| clipboard.ts | Clipboard utilities | No |
| clipboard-image.ts | Clipboard image handling | No |
| clipboard-native.ts | Native clipboard | No |
| exif-orientation.ts | EXIF orientation | No |
| frontmatter.ts | Frontmatter parsing | No |
| fs-watch.ts | File system watching | No |
| git.ts | Git utilities | No |
| image-convert.ts | Image conversion | No |
| image-resize.ts | Image resize | No |
| mime.ts | MIME type detection | No |
| paths.ts | Path utilities | No |
| photon.ts | Photon utilities | No |
| pi-user-agent.ts | Pi user agent | No |
| shell.ts | Shell utilities | No |
| sleep.ts | Sleep utility | No |
| tools-manager.ts | Tools manager | No |
| version-check.ts | Version check | No |
| child-process.ts | Child process utilities | No |
| changelog.ts | Changelog utilities | No |

---

## 18. Main Entry Point (main.ts)

| Feature | TypeScript | C++ Port |
|---------|-----------|----------|
| main() | Full orchestration | Partial (main.cpp + agent.cpp) |
| MainOptions | Main options type | No |

---

## Summary

The C++ port has the following core infrastructure that the TypeScript original does not need to expose:
- LlamaCpp provider (C++ native)
- Linear JSONL session store (SessionStore) with `compaction`/`branch_summary`/`compaction_skipped` rows
- Basic tools (bash, edit, find, grep, ls, read, write)
- Basic agent loop with compaction
- Readline interactive and print modes (including `/compact`, `/stats`, token budget status, and cancellation)
- `AgentSession` interface scaffolding (`agent_session.hpp`) not yet integrated in runtime

**Missing from the port (by priority):**

1. **AgentSession runtime integration** - `agent_session.hpp` exists, but `agent.cpp` still drives execution through `run_agent_loop(...)`.
2. **SessionManager tree model** - Full branch/leaf traversal, labels, and typed tree entries remain unimplemented (current store is linear JSONL).
3. **Extension system** - ExtensionRunner, ExtensionContext, ExtensionAPI, hooks, and extension commands remain missing.
4. **AuthStorage + OAuth** - Credential backends and OAuth auth flows are not implemented.
5. **ModelRegistry** - Provider registration, API key model discovery, and OAuth-aware model listing are missing.
6. **Queue management and retry** - steer/followUp queues and exponential backoff retry logic are not implemented.
7. **Session-level bash execution API** - C++ has a bash tool, but no session-level streaming/abort executor abstraction equivalent to TS.
8. **Interactive UI parity** - TS selector/dialog/component surface is still largely absent.
9. **Theme system parity** - TS theme/color/highlighting modules are not ported.
10. **RPC mode** - JSONL RPC server/client mode is not implemented.
11. **Session export parity** - HTML/JSONL export helpers are missing.
12. **Tooling infrastructure parity** - File mutation queue, tool wrappers, and TS render/truncation helper modules are missing.
13. **Prompt templates and skills system** - Template expansion and `/skill` command expansion are not implemented.
14. **SDK factories** - Programmatic factories from `core/sdk.ts` are not ported.
15. **Core utility parity** - Event bus and several support utilities remain missing or partial.

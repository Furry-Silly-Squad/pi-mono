# Phase 7: Missing Functionality (TypeScript -> C++ Port)

This file catalogs functionality present in `packages/coding-agent/src` that is not yet implemented in `ports/coding-agent/src`.

---

## 1. Agent Session (agent-session.ts)

The C++ port has a flat `agent_loop.cpp` with no session abstraction. The TypeScript `AgentSession` class is the core abstraction.

| Feature | TypeScript | C++ Port |
|---------|-----------|----------|
| AgentSession class | Full lifecycle, state, events | No equivalent |
| Event subscription system (subscribe, _handleAgentEvent, _emit) | Yes | No |
| Agent state (state, model, thinkingLevel, isStreaming, systemPrompt) | Yes | No |
| Model management (setModel, cycleModel, cycleThinkingLevel, setThinkingLevel) | Yes | No |
| Thinking levels (off, minimal, low, medium, high, xhigh) | Yes | No |
| Queue management (steer, followUp, clearQueue) | Yes | No |
| Auto-compaction (overflow recovery, threshold-based) | Yes | Partial (manual only in agent_loop) |
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
| Tool registry management (getActiveToolNames, setActiveToolsByName, getAllTools) | Yes | No |
| Context usage tracking (getContextUsage) | Yes | No |
| Model cycling with scoped models | Yes | No |

---

## 2. Session Manager (session-manager.ts)

The C++ `SessionStore` is a simple JSONL append/read. The TypeScript `SessionManager` has a full tree data structure.

| Feature | TypeScript | C++ Port |
|---------|-----------|----------|
| SessionManager class | Full tree traversal, branching | No (SessionStore is flat) |
| Session entry types (message, compaction, custom, branchSummary, bashExecution, modelChange, thinkingLevelChange, sessionInfo, label) | 9+ entry types | Only message, compaction, branch_summary, compaction_skipped |
| Branch/leaf management (branch, resetLeaf, getLeafId) | Yes | No |
| Custom entries (compactionSummary, branchSummary, bashExecution, modelChange, thinkingLevelChange, sessionInfo, label) | Yes | No |
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
| SettingsManager class | Full settings persistence | No |
| Compaction settings (enabled, reserveTokens, keepRecentTokens) | Yes | Config-level only |
| Retry settings (enabled, maxRetries, baseDelayMs) | Yes | No |
| Image settings (autoResize) | Yes | No |
| Shell settings (commandPrefix, shellPath) | Yes | No |
| Default model/provider persistence | Yes | No |
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
| Compaction settings (from SettingsManager) | Yes | Config-level only |
| Auto-compaction (overflow + threshold) | Yes | No |
| Compaction failure handling (fail-fast vs graceful skip) | Yes | Partial (fail-fast only) |

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

The C++ port has a basic `interactive_mode.cpp`. The TypeScript version has 30+ UI components.

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
| runPrintMode | Print mode implementation | Partial (print_mode.cpp exists but simpler) |

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
- Simple JSONL session store (SessionStore)
- Basic tools (bash, edit, find, grep, ls, read, write)
- Basic agent loop with compaction
- Basic interactive and print modes

**Missing from the port (by priority):**

1. **AgentSession abstraction** - The entire AgentSession class is missing. This is the core session abstraction.
2. **SessionManager** - Full tree data structure with branching, labels, and entry types.
3. **Extension system** - ExtensionRunner, ExtensionContext, ExtensionAPI, hooks, commands.
4. **SettingsManager** - Persistent settings for compaction, retry, images, shell, theme, etc.
5. **AuthStorage** - OAuth and API key credential management.
6. **ModelRegistry** - API key resolution and model discovery.
7. **Auto-retry** - Exponential backoff for retryable errors.
8. **Queue management** - steer/followUp for streaming sessions.
9. **Bash execution** - Session-level bash with streaming and abort.
10. **Interactive mode components** - 30+ UI components (footer, selectors, dialogs, etc.).
11. **Theme system** - Full theme with colors and highlighting.
12. **RPC mode** - JSONL RPC protocol.
13. **HTML/JSONL export** - Session export functionality.
14. **CLI utilities** - Config selector, session picker, model listing, etc.
15. **Tool system enhancements** - File mutation queue, edit diffs, render utilities.
16. **Skills system** - Skills loading and expansion.
17. **Prompt templates** - File-based prompt template expansion.
18. **SDK factories** - Programmatic session/tool creation.
19. **Event bus** - Centralized event dispatching.
20. **Resource loader** - Skills, prompts, themes, context files, system prompt.

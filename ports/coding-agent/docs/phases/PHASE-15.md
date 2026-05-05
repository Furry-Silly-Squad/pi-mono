# PHASE-15: Settings Manager

## Goal

Add a structured `SettingsManager` to the C++ port that persists and manages agent configuration across sessions, matching the TypeScript `SettingsManager` for the settings that are actually used in the C++ port. This enables:

1. **Persistent settings** — model, thinking level, compaction, retry, shell config, and other agent parameters persist across sessions in a structured JSON file.
2. **Settings overrides** — CLI args and `settings.json` are merged at startup; runtime changes are persisted back.
3. **Settings migrations** — handle format changes from older `settings.json` versions.
4. **In-memory settings** — support for testing and ephemeral sessions without file I/O.

## TypeScript Reference

The TS `SettingsManager` (`packages/coding-agent/src/core/settings-manager.ts`) is a comprehensive settings system:

- **Two scopes**: `global` (`~/.config/coding-agent/settings.json`) and `project` (`<cwd>/.pi/settings.json`).
- **Deep merge**: project settings override global settings; nested objects merge recursively.
- **Lockfile protection**: `proper-lockfile` prevents concurrent corruption.
- **Migration**: `migrateSettings()` handles legacy format changes (e.g., `queueMode` → `steeringMode`, `websockets` boolean → `transport` enum).
- **Modified field tracking**: only writes fields that changed during the session.
- **Async write queue**: serializes writes to avoid race conditions.
- **Many settings fields**: compaction, retry, thinking budgets, shell config, themes, packages, skills, prompts, extensions, terminal, images, and more.

The C++ port only uses a subset of these settings. The phase focuses on the fields that actually affect the C++ agent loop.

## Current C++ State

The C++ port has partial settings support:

- **`Config` struct** (`config.hpp`) — holds CLI-parsed and settings-merged values:
  - `model`, `api_key`, `temperature`, `max_tokens`, `context_size`
  - `compaction_reserve_tokens`, `compaction_keep_recent_tokens`, `compaction_fail_fast`
  - `initial_active_tools`, `tool_execution_mode`
  - `retry_enabled`, `retry_max_retries`, `retry_base_delay_ms`, `retry_max_retry_delay_ms`, `retry_timeout_ms`
  - `interactive_debug`, `max_empty_completion_nudges`
  - `branch_summary`
- **`settings.json` loading** (`config.cpp`) — reads `~/.config/coding-agent/settings.json` and merges a handful of fields into `Config`.
- **`AgentSessionConfig`** (`agent_session.hpp`) — extends `Config` with session-specific settings and callbacks.

**What's missing:**

1. **No structured settings manager** — settings are merged into `Config` at startup and never persisted back.
2. **No thinking level persistence** — `ThinkingLevel` is set at runtime but not saved.
3. **No shell path / command prefix** — not yet added (PHASE-13).
4. **No model persistence** — `model` is in `Config` but not persisted as a default.
5. **No steering/follow-up mode persistence** — not yet implemented.
6. **No settings migrations** — no handling of format changes.
7. **No in-memory settings** — no way to test settings without file I/O.
8. **No settings reload** — no way to reload settings during a session.

## Design

### 1. Settings Storage Interface

Define an abstract storage interface that supports scoped read/write with locking:

```cpp
/// Scope of settings: global (user-wide) or project (cwd-local).
enum class SettingsScope { Global, Project };

/// Abstract settings storage backend.
class SettingsStorage {
public:
    virtual ~SettingsStorage() = default;

    /// Read the current settings JSON string for a scope.
    /// Returns empty string if no settings file exists.
    virtual std::string read(SettingsScope scope) = 0;

    /// Write a new settings JSON string for a scope.
    /// The implementation handles locking and directory creation.
    virtual void write(SettingsScope scope, const std::string& json) = 0;
};
```

### 2. File-Based Storage

Implement `FileSettingsStorage` that reads/writes JSON files with basic file locking:

```cpp
/// File-based settings storage with basic locking.
/// Global: ~/.config/coding-agent/settings.json
/// Project: <cwd>/.pi/settings.json
class FileSettingsStorage final : public SettingsStorage {
public:
    FileSettingsStorage(const std::string& agent_dir, const std::string& cwd);

    std::string read(SettingsScope scope) override;
    void write(SettingsScope scope, const std::string& json) override;

private:
    std::string global_settings_path_;
    std::string project_settings_path_;
    std::string cwd_;
};
```

Path resolution:
- **Global**: `~/.config/coding-agent/settings.json` (same as current C++ behavior).
- **Project**: `<cwd>/.pi/settings.json` (new, mirrors TS `CONFIG_DIR_NAME`).

Locking: Use a simple `.lock` file with `flock()` on Unix. On platforms without `flock()`, fall back to `mkdir`-based locking or no locking (documented limitation).

```cpp
/// Acquire a file lock with retry.
/// Returns a unique_ptr that releases the lock on destruction.
std::unique_ptr<std::FILE> acquire_lock(const std::string& path);
```

### 3. In-Memory Storage

Implement `InMemorySettingsStorage` for testing:

```cpp
/// In-memory settings storage (no file I/O).
class InMemorySettingsStorage final : public SettingsStorage {
public:
    std::string read(SettingsScope scope) override;
    void write(SettingsScope scope, const std::string& json) override;

private:
    std::map<SettingsScope, std::string> storage_;
};
```

### 4. Settings Struct

Define a `Settings` struct that mirrors the subset of TS `Settings` used by the C++ port:

```cpp
struct CompactionSettings {
    bool enabled = true;
    int reserve_tokens = 16384;
    int keep_recent_tokens = 20000;
};

struct BranchSummarySettings {
    int reserve_tokens = 16384;
    bool skip_prompt = false;
};

struct RetrySettings {
    bool enabled = true;
    int max_retries = 3;
    int base_delay_ms = 1000;
    int max_retry_delay_ms = 60000;
    int timeout_ms = 30000;
};

enum class ThinkingLevelSetting { Off, Minimal, Low, Medium, High, XHigh };

enum class QueueMode { All, OneAtATime };

struct Settings {
    // Model
    std::string default_provider;      // "" = use CLI default
    std::string default_model;

    // Thinking
    ThinkingLevelSetting default_thinking_level = ThinkingLevelSetting::Off;

    // Queue modes
    QueueMode steering_mode = QueueMode::OneAtATime;
    QueueMode follow_up_mode = QueueMode::OneAtATime;

    // Compaction
    CompactionSettings compaction;

    // Branch summary
    BranchSummarySettings branch_summary;

    // Retry
    RetrySettings retry;

    // Shell
    std::string shell_path;            // "" = auto-detect
    std::string shell_command_prefix;  // "" = no prefix

    // Other
    bool hide_thinking_block = false;
    bool quiet_startup = false;
    bool interactive_debug = true;
    int max_empty_completion_nudges = 2;
    std::string theme;                 // "" = no theme
    std::string session_dir;           // "" = default (~/.local/share/coding-agent/sessions)
};
```

### 5. Settings Manager

The core `SettingsManager` class:

```cpp
class SettingsManager {
public:
    /// Create a SettingsManager that loads from files.
    static std::unique_ptr<SettingsManager> create(
        const std::string& agent_dir,
        const std::string& cwd
    );

    /// Create a SettingsManager from an arbitrary storage backend.
    static std::unique_ptr<SettingsManager> fromStorage(
        std::unique_ptr<SettingsStorage> storage
    );

    /// Create an in-memory SettingsManager with optional initial settings.
    static std::unique_ptr<SettingsManager> inMemory(
        const Settings& initial = {}
    );

    // ==================================================================
    // Read Access (merged: project overrides global)
    // ==================================================================

    const Settings& get() const;
    const Settings& getGlobal() const;
    const Settings& getProject() const;

    // Model
    std::string getDefaultProvider() const;
    std::string getDefaultModel() const;

    // Thinking
    ThinkingLevelSetting getDefaultThinkingLevel() const;

    // Queue modes
    QueueMode getSteeringMode() const;
    QueueMode getFollowUpMode() const;

    // Compaction
    bool getCompactionEnabled() const;
    int getCompactionReserveTokens() const;
    int getCompactionKeepRecentTokens() const;

    // Branch summary
    int getBranchSummaryReserveTokens() const;
    bool getBranchSummarySkipPrompt() const;

    // Retry
    bool getRetryEnabled() const;
    int getRetryMaxRetries() const;
    int getRetryBaseDelayMs() const;
    int getRetryMaxRetryDelayMs() const;
    int getRetryTimeoutMs() const;

    // Shell
    std::string getShellPath() const;
    std::string getShellCommandPrefix() const;

    // Other
    bool getHideThinkingBlock() const;
    bool getQuietStartup() const;
    bool getInteractiveDebug() const;
    int getMaxEmptyCompletionNudges() const;
    std::string getTheme() const;
    std::string getSessionDir() const;

    // ==================================================================
    // Write Access (persist to global settings)
    // ==================================================================

    void setDefaultProvider(const std::string& provider);
    void setDefaultModel(const std::string& model);
    void setDefaultThinkingLevel(ThinkingLevelSetting level);
    void setSteeringMode(QueueMode mode);
    void setFollowUpMode(QueueMode mode);
    void setCompactionEnabled(bool enabled);
    void setBranchSummarySkipPrompt(bool skip);
    void setRetryEnabled(bool enabled);
    void setShellPath(const std::string& path);
    void setShellCommandPrefix(const std::string& prefix);
    void setHideThinkingBlock(bool hide);
    void setQuietStartup(bool quiet);
    void setInteractiveDebug(bool debug);
    void setMaxEmptyCompletionNudges(int n);
    void setTheme(const std::string& theme);
    void setSessionDir(const std::string& dir);

    // ==================================================================
    // Reload & Flush
    // ==================================================================

    /// Reload settings from disk.
    void reload();

    // ==================================================================
    // Apply CLI overrides (runtime-only, not persisted)
    // ==================================================================

    /// Apply additional overrides on top of current merged settings.
    void applyOverrides(const Settings& overrides);

    // ==================================================================
    // Migration
    // ==================================================================

    /// Migrate old settings format to new format.
    static Settings migrateSettings(const json& raw);

private:
    SettingsManager(
        std::unique_ptr<SettingsStorage> storage,
        Settings global,
        Settings project
    );

    void saveGlobal();
    void saveProject(const Settings& project_settings);
    Settings mergeSettings() const;

    std::unique_ptr<SettingsStorage> storage_;
    Settings global_settings_;
    Settings project_settings_;
    Settings merged_settings_;

    // Track modified fields for selective writes
    std::set<std::string> modified_global_fields_;
    std::set<std::string> modified_project_fields_;
};
```

### 6. Settings → Config Bridge

Add a helper to convert `Settings` to `Config` / `AgentSessionConfig`:

```cpp
/// Apply settings defaults to a Config.
/// CLI args take precedence over settings values.
Config applySettingsToConfig(const Config& cli_config, const Settings& settings);

/// Apply settings defaults to an AgentSessionConfig.
AgentSessionConfig applySettingsToSessionConfig(
    const AgentSessionConfig& session_config,
    const Settings& settings
);
```

Logic:
1. Start from the CLI-parsed `Config`.
2. For each field, if the CLI value equals the default (unset), use the settings value.
3. This ensures CLI args always win, settings provide defaults.

### 7. Settings Migrations

Implement `migrateSettings()` to handle known format changes:

```cpp
Settings SettingsManager::migrateSettings(const json& raw) {
    Settings result;

    // Migrate queueMode -> steeringMode
    if (raw.contains("queueMode") && !raw.contains("steeringMode")) {
        result.steering_mode = raw["queueMode"].get<std::string>() == "all"
            ? QueueMode::All : QueueMode::OneAtATime;
        raw.erase("queueMode");
    }

    // Migrate legacy websockets boolean -> transport (no-op in C++ port)
    // (C++ port doesn't support websockets, skip)

    // Migrate retry.maxDelayMs -> retry.max_retry_delay_ms
    if (raw.contains("retry") && raw["retry"].is_object()) {
        auto& retry = raw["retry"];
        if (retry.contains("maxDelayMs") && !retry.contains("maxRetryDelayMs")) {
            retry["maxRetryDelayMs"] = retry["maxDelayMs"];
            retry.erase("maxDelayMs");
        }
    }

    return result;
}
```

### 8. Integration with Config

Modify `config.cpp` to use `SettingsManager` instead of ad-hoc `settings.json` loading:

```cpp
std::optional<Config> parse_config(int argc, char** argv, std::string& error) {
    // 1. Parse CLI args into a Config with all defaults.
    Config config = parse_cli_args(argc, argv, error);
    if (!config) return std::nullopt;

    // 2. Load settings.json via SettingsManager.
    const char* home = std::getenv("HOME");
    if (home != nullptr) {
        auto settings = SettingsManager::create(
            std::string(home) / ".config" / "coding-agent",
            config->cwd
        );

        // 3. Apply settings defaults (CLI wins).
        config = applySettingsToConfig(config.value(), *settings);
    }

    return config;
}
```

### 9. Integration with AgentSession

Pass `SettingsManager` (or a reference) to `AgentSession` for runtime settings changes:

```cpp
struct AgentSessionConfig {
    // ... existing fields ...

    /// Optional settings manager for runtime settings changes.
    /// When set, the agent can update settings (e.g., thinking level changes via /thinking).
    std::weak_ptr<SettingsManager> settings_manager;
};
```

The interactive mode will use this to persist settings changes from slash commands:

```cpp
// In interactive_mode.cpp, for /thinking:
case Command::Thinking: {
    auto level = cycle_thinking_level(forward);
    if (settings_manager) {
        settings_manager->setDefaultThinkingLevel(level);
    }
    break;
}
```

## File Layout

```
ports/coding-agent/src/
├── settings.hpp              # NEW: SettingsStorage, Settings, SettingsManager
├── settings.cpp              # NEW: FileSettingsStorage, InMemorySettingsStorage, SettingsManager
├── config.hpp                # MODIFY: remove redundant fields (moved to Settings)
├── config.cpp                # MODIFY: use SettingsManager for settings loading
├── agent_session.hpp         # MODIFY: add settings_manager to AgentSessionConfig
├── agent_session.cpp         # MODIFY: apply settings for runtime changes
└── modes/
    ├── interactive_mode.cpp  # MODIFY: persist settings changes from slash commands
    └── interactive_mode.hpp  # No changes
```

## Settings Field Mapping (C++ vs TS)

| TS Setting | C++ Equivalent | Notes |
|---|---|---|
| `defaultProvider` | `default_provider` | Provider selection |
| `defaultModel` | `default_model` | Model selection |
| `defaultThinkingLevel` | `default_thinking_level` | Thinking level |
| `steeringMode` | `steering_mode` | Queue mode |
| `followUpMode` | `follow_up_mode` | Queue mode |
| `compaction.enabled` | `compaction.enabled` | Compaction toggle |
| `compaction.reserveTokens` | `compaction.reserve_tokens` | Compaction reserve |
| `compaction.keepRecentTokens` | `compaction.keep_recent_tokens` | Compaction keep recent |
| `branchSummary.reserveTokens` | `branch_summary.reserve_tokens` | Branch summary reserve |
| `branchSummary.skipPrompt` | `branch_summary.skip_prompt` | Skip branch summary prompt |
| `retry.enabled` | `retry.enabled` | Retry toggle |
| `retry.maxRetries` | `retry.max_retries` | Max retries |
| `retry.baseDelayMs` | `retry.base_delay_ms` | Base delay |
| `retry.maxRetryDelayMs` | `retry.max_retry_delay_ms` | Max delay |
| `retry.timeoutMs` | `retry.timeout_ms` | Timeout |
| `shellPath` | `shell_path` | Shell path (PHASE-13) |
| `shellCommandPrefix` | `shell_command_prefix` | Command prefix (PHASE-13) |
| `hideThinkingBlock` | `hide_thinking_block` | Hide thinking output |
| `quietStartup` | `quiet_startup` | Suppress startup banner |
| (implicit) | `interactive_debug` | Turn diagnostics |
| `max_empty_completion_nudges` | `max_empty_completion_nudges` | Empty completion nudges |
| `theme` | `theme` | Theme name |
| `sessionDir` | `session_dir` | Custom session directory |
| `transport` | — | Not used in C++ (single provider) |
| `packages` | — | Not used in C++ |
| `extensions` | — | Not used in C++ |
| `skills` | — | Not used in C++ |
| `prompts` | — | Not used in C++ |
| `terminal.*` | — | TUI-specific, not used |
| `images.*` | — | Image handling, not used |
| `thinkingBudgets` | — | Not used in C++ |
| `markdown.*` | — | Not used in C++ |

## C++ vs TS Differences

| Aspect | TS | C++ (target) |
|---|---|---|
| Scopes | Global + project | Global only (project scope deferred) |
| Locking | `proper-lockfile` (npm package) | Simple `.lock` file with `flock()` |
| Write strategy | Async queue, selective field writes | Sync write, selective field writes |
| Migration | `migrateSettings()` in constructor | `migrateSettings()` static, called during load |
| Error handling | `SettingsError[]` array, `drainErrors()` | Log to stderr, no error array |
| Nested object handling | Recursive deep merge | Flat struct (no nested objects in Settings) |
| In-memory | `InMemorySettingsStorage` | `InMemorySettingsStorage` (same) |

**Rationale for simplifications:**
- **No project scope**: The C++ port is a single-binary CLI. Project-scoped settings are a TUI/ecosystem feature. Can be added later.
- **Sync writes**: The C++ port doesn't have a full async runtime. Sync writes with locking are sufficient for a CLI tool.
- **No error array**: Settings errors are logged to stderr at load time. Runtime errors (write failures) are also logged. No need for an error queue.
- **Flat struct**: The C++ `Settings` uses flat structs (`CompactionSettings`, `RetrySettings`) rather than deeply nested objects, making serialization simpler.

## Implementation Order

1. **`Settings` struct + `SettingsStorage` interface** (`settings.hpp`) — types and interfaces.
2. **`FileSettingsStorage`** (`settings.cpp`) — file I/O with basic locking.
3. **`InMemorySettingsStorage`** — in-memory backend for testing.
4. **`SettingsManager` core** — load, merge, read, write, migrate.
5. **Settings → Config bridge** — `applySettingsToConfig()`.
6. **Update `config.cpp`** — use `SettingsManager` instead of ad-hoc loading.
7. **Update `AgentSessionConfig`** — add `settings_manager` weak pointer.
8. **Interactive mode integration** — persist settings changes from `/thinking`, `/compact`, `/retry`, etc.

## Testing

- Unit tests for `FileSettingsStorage` — read/write/lock behavior.
- Unit tests for `InMemorySettingsStorage` — basic read/write.
- Unit tests for `SettingsManager::create()` — load from file, handle missing file, handle parse errors.
- Unit tests for `SettingsManager::migrateSettings()` — each migration case.
- Unit tests for `applySettingsToConfig()` — CLI wins over settings, settings provide defaults.
- Unit tests for `SettingsManager` write operations — verify JSON serialization, selective field writes.
- Integration test: start agent, change thinking level via `/thinking`, verify `settings.json` updated.
- Integration test: start agent, change compaction via `/compact`, verify `settings.json` updated.

## Dependencies

- No new dependencies. Uses existing nlohmann/json, stdlib.
- `flock()` on POSIX systems for basic locking.
- `std::filesystem` (C++17, already used in `config.cpp`).

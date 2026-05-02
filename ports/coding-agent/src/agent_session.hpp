#pragma once

#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "compaction.hpp"
#include "config.hpp"
#include "file_ops.hpp"
#include "providers/provider.hpp"
#include "session_entry.hpp"
#include "tools/tool_registry.hpp"

namespace coding_agent {

// ============================================================================
// Thinking Level
// ============================================================================

enum class ThinkingLevel {
    Off,
    Minimal,
    Low,
    Medium,
    High,
    XHigh,
};

const char* thinking_level_to_string(ThinkingLevel level);
ThinkingLevel string_to_thinking_level(const std::string& str);

// ============================================================================
// Agent Events
// ============================================================================

struct AgentEvent {
    enum class Type {
        TurnStart,
        TurnEnd,
        ModelCallStart,
        ToolCall,
        ToolResult,
        ModelChange,
        ThinkingLevelChange,
        CompactionStart,
        CompactionEnd,
        Error,
        Abort,
    };

    Type type;

    // For TurnStart/TurnEnd
    int turn_index = 0;

    // For ToolCall
    std::string tool_name;
    std::string tool_call_id;
    std::string tool_args;

    // For ToolResult
    std::string tool_result;
    bool tool_error = false;

    // For ModelChange
    std::string previous_model;
    std::string new_model;

    // For ThinkingLevelChange
    ThinkingLevel previous_thinking_level = ThinkingLevel::Off;
    ThinkingLevel new_thinking_level = ThinkingLevel::Off;

    // For CompactionEnd
    bool compaction_success = false;
    int tokens_before = 0;
    int tokens_after = 0;
    std::string compaction_summary;
    std::string compaction_error;

    // For Error
    std::string error_message;
};

using AgentEventHandler = std::function<void(const AgentEvent&)>;

// ============================================================================
// AgentSessionConfig - extended from Config with session-specific settings
// ============================================================================

struct AgentSessionConfig {
    // From Config
    std::string provider;
    std::string base_url;
    std::string model;
    std::string api_key;
    std::string cwd;
    std::optional<std::string> system_prompt_path;
    std::vector<std::string> append_system_prompts;
    std::optional<std::string> session_id;
    int max_tokens;
    int context_size;
    int compaction_reserve_tokens;
    int compaction_keep_recent_tokens;
    int max_tool_iterations;
    float temperature;
    bool stream;
    bool no_tools;
    bool no_context_files;
    bool compaction_fail_fast;

    // Session-specific settings
    bool auto_compaction = true;
    std::string initial_active_tools = "read,bash,edit,write";

    // Callbacks
    AgentEventHandler on_event;
};

// ============================================================================
// AgentSession - Core session abstraction
// ============================================================================

class AgentSession {
 public:
    AgentSession(const AgentSessionConfig& config,
                 Provider& provider,
                 ToolRegistry& tools,
                 SessionManager& session);

    ~AgentSession();

    // Non-copyable, non-movable
    AgentSession(const AgentSession&) = delete;
    AgentSession& operator=(const AgentSession&) = delete;
    AgentSession(AgentSession&&) = delete;
    AgentSession& operator=(AgentSession&&) = delete;

    // ====================================================================
    // Run Loop
    // ====================================================================

    /// Run the agent loop with user input. Returns true on success, false on error/abort.
    bool run(const std::string& user_input,
             const ChunkCallback& on_chunk,
             std::atomic<bool>* cancel_flag = nullptr);

    /// Abort the current operation. Thread-safe.
    void abort();

    /// Check if agent is currently running/streaming.
    bool is_running() const;

    // ====================================================================
    // State Access
    // ====================================================================

    /// Current model ID.
    const std::string& model() const;

    /// Current thinking level.
    ThinkingLevel thinking_level() const;

    /// Current system prompt.
    const std::string& system_prompt() const;

    /// Current messages (read-only).
    const std::vector<ChatMessage>& messages() const;

    /// Number of messages in history.
    size_t message_count() const;

    /// Current active tool names.
    std::vector<std::string> active_tools() const;

    // ====================================================================
    // Model Management
    // ====================================================================

    /// Set model directly.
    bool set_model(const std::string& model_id);

    /// Cycle to next model (returns true if model changed).
    bool cycle_model(bool forward = true);

    /// Set thinking level.
    void set_thinking_level(ThinkingLevel level);

    /// Cycle to next thinking level (returns true if level changed).
    bool cycle_thinking_level(bool forward = true);

    /// Get available thinking levels for current model.
    std::vector<ThinkingLevel> available_thinking_levels() const;

    // ====================================================================
    // Tool Management
    // ====================================================================

    /// Set active tools by name.
    void set_active_tools(const std::vector<std::string>& tool_names);

    /// Get all registered tool definitions.
    std::vector<ToolDefinition> all_tool_definitions() const;

    // ====================================================================
    // Compaction
    // ====================================================================

    /// Manually trigger compaction. Returns true on success.
    bool compact();

    /// Abort in-progress compaction.
    void abort_compaction();

    /// Check if compaction is currently running.
    bool is_compacting() const;

    /// Get last compaction stats.
    const CompactionStats& last_compaction_stats() const;

    // ====================================================================
    // Event Handling
    // ====================================================================

    /// Set event handler.
    void set_event_handler(AgentEventHandler handler);

    // ====================================================================
    // Session Info
    // ====================================================================

    /// Read-only access to session configuration.
    const AgentSessionConfig& session_config() const;

    /// Get current session ID.
    std::string session_id() const;

    /// Get session file path.
    std::string session_path() const;

    /// Get compaction count.
    int compaction_count() const;

    /// Get total context tokens.
    int total_context_tokens() const;

 private:
    // ====================================================================
    // Internal Run Loop
    // ====================================================================

    bool run_turn(const std::string& user_input,
                  const ChunkCallback& on_chunk,
                  std::atomic<bool>* cancel_flag);

    bool call_provider(const std::vector<ChatMessage>& history,
                       const std::vector<ToolDefinition>& tools,
                       ChatResponse& response,
                       std::string& error,
                       const ChunkCallback& on_chunk,
                       std::atomic<bool>* cancel_flag);

    bool execute_tools(const std::vector<ToolCall>& tool_calls,
                       const ChunkCallback& on_chunk);

    std::vector<ToolDefinition> active_tool_definitions() const;

    bool check_and_compact(const ChunkCallback& on_chunk);

    // ====================================================================
    // Event Helpers
    // ====================================================================

    void emit_event(AgentEvent::Type type, const AgentEvent& event);

    // ====================================================================
    // State
    // ====================================================================

    AgentSessionConfig config_;
    Provider& provider_;
    ToolRegistry& tools_;
    SessionManager& session_;

    std::vector<ChatMessage> messages_;
    FileOps last_compaction_file_ops_;
    std::string current_model_;
    ThinkingLevel current_thinking_level_ = ThinkingLevel::Off;
    std::string current_system_prompt_;
    std::vector<std::string> active_tool_names_;

    std::atomic<bool> running_{false};
    std::atomic<bool> compacting_{false};
    std::atomic<bool> abort_requested_{false};

    AgentEventHandler event_handler_;
    CompactionStats last_compaction_stats_;
    int compaction_count_ = 0;
    int turn_index_ = 0;
};

}  // namespace coding_agent

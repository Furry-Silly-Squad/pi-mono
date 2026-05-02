#pragma once

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

#include <nlohmann/json.hpp>

#include "compaction.hpp"
#include "providers/provider.hpp"

namespace coding_agent {

// ============================================================================
// Current session file version
// ============================================================================

inline constexpr int CURRENT_SESSION_VERSION = 3;

// ============================================================================
// Session header
// ============================================================================

struct SessionHeader {
  std::string type = "session";
  int version = CURRENT_SESSION_VERSION;
  std::string id;
  std::string timestamp;
  std::string cwd;
  std::optional<std::string> parentSession;
};

// ============================================================================
// Base entry with common fields
// ============================================================================

struct SessionEntryBase {
  std::string type;
  std::string id;
  std::string parentId;
  std::string timestamp;
};

// ============================================================================
// Message entry
// ============================================================================

struct SessionMessageEntry : SessionEntryBase {
  std::string type = "message";
  ChatMessage message;
};

// ============================================================================
// Thinking level change entry
// ============================================================================

struct ThinkingLevelChangeEntry : SessionEntryBase {
  std::string type = "thinking_level_change";
  std::string thinkingLevel;
};

// ============================================================================
// Model change entry
// ============================================================================

struct ModelChangeEntry : SessionEntryBase {
  std::string type = "model_change";
  std::string provider;
  std::string modelId;
};

// ============================================================================
// Compaction entry
// ============================================================================

struct CompactionEntry : SessionEntryBase {
  std::string type = "compaction";
  std::string summary;
  std::string firstKeptEntryId;
  int tokensBefore = 0;
  std::optional<nlohmann::json> details;
  bool fromHook = false;
};

// ============================================================================
// Branch summary entry
// ============================================================================

struct BranchSummaryEntry : SessionEntryBase {
  std::string type = "branch_summary";
  std::string fromId;
  std::string summary;
  std::optional<nlohmann::json> details;
  bool fromHook = false;
};

// ============================================================================
// Custom entry (for extensions — does NOT participate in LLM context)
// ============================================================================

struct CustomEntry : SessionEntryBase {
  std::string type = "custom";
  std::string customType;
  std::optional<nlohmann::json> data;
};

// ============================================================================
// Label entry (user-defined bookmarks/markers on entries)
// ============================================================================

struct LabelEntry : SessionEntryBase {
  std::string type = "label";
  std::string targetId;
  std::optional<std::string> label;
};

// ============================================================================
// Session info entry (user-defined display name)
// ============================================================================

struct SessionInfoEntry : SessionEntryBase {
  std::string type = "session_info";
  std::optional<std::string> name;
};

// ============================================================================
// Custom message entry (for extensions — DOES participate in LLM context)
// ============================================================================

struct CustomMessageEntry : SessionEntryBase {
  std::string type = "custom_message";
  std::string customType;
  std::string content;
  bool display = false;
  std::optional<nlohmann::json> details;
};

// ============================================================================
// Session entry — tagged union of all entry types (non-header)
// ============================================================================

using SessionEntry = std::variant<
    SessionMessageEntry,
    ThinkingLevelChangeEntry,
    ModelChangeEntry,
    CompactionEntry,
    BranchSummaryEntry,
    CustomEntry,
    LabelEntry,
    SessionInfoEntry,
    CustomMessageEntry
>;

// ============================================================================
// Raw file entry (includes header)
// ============================================================================

using FileEntry = std::variant<SessionHeader, SessionEntry>;

// ============================================================================
// Tree node for getTree()
// ============================================================================

struct SessionTreeNode {
  SessionEntry entry;
  std::vector<SessionTreeNode> children;
  std::optional<std::string> label;
  std::optional<std::string> labelTimestamp;
};

// ============================================================================
// Session context (what gets sent to the LLM)
// ============================================================================

struct SessionContext {
  std::vector<ChatMessage> messages;
  std::string thinkingLevel;
  struct {
    std::string provider;
    std::string modelId;
  } model;
};

// ============================================================================
// Session info (metadata about a session file)
// ============================================================================

struct SessionInfo {
  std::string path;
  std::string id;
  std::string cwd;
  std::optional<std::string> name;
  std::optional<std::string> parentSessionPath;
  std::string created;
  std::string modified;
  int messageCount = 0;
  std::string firstMessage;
  std::string allMessagesText;
};

// ============================================================================
// Session progress callback
// ============================================================================

using SessionListProgress = std::function<void(int loaded, int total)>;

/// Root directory used by `SessionManager` (`~/.pi/agent/sessions`).
std::string agent_sessions_root_directory();

/// Per-workspace session directory (same layout as `SessionManager::create(..., "")`).
std::string session_directory_for_cwd(const std::string& cwd);

// ============================================================================
// SessionManager — manages conversation sessions as append-only trees
// ============================================================================

class SessionManager {
 public:
  // --------------------------------------------------------------------------
  // Factory constructors
  // --------------------------------------------------------------------------

  /// Create a new session in the default session directory for `cwd`.
  static std::unique_ptr<SessionManager> create(const std::string& cwd,
                                                 const std::string& sessionDir = "");

  /// Open an existing session file.
  static std::unique_ptr<SessionManager> open(const std::string& path,
                                               const std::string& sessionDir = "",
                                               const std::string& cwdOverride = "");

  /// Continue the most recent session, or create new if none.
  static std::unique_ptr<SessionManager> continueRecent(const std::string& cwd,
                                                         const std::string& sessionDir = "");

  /// Create an in-memory session (no file persistence).
  static std::unique_ptr<SessionManager> inMemory(const std::string& cwd = "");

  /// Fork a session from another project directory.
  static std::unique_ptr<SessionManager> forkFrom(const std::string& sourcePath,
                                                   const std::string& targetCwd,
                                                   const std::string& sessionDir = "");

  /// Open an existing session whose header `id` matches `sessionId`, or `sessionDir/id.jsonl` if present.
  static std::unique_ptr<SessionManager> openBySessionId(const std::string& cwd,
                                                          const std::string& sessionId,
                                                          const std::string& sessionDir = "");

  // --------------------------------------------------------------------------
  // Session lifecycle
  // --------------------------------------------------------------------------

  struct NewSessionOptions {
    std::optional<std::string> id;
    std::optional<std::string> parentSession;
  };

  /// Start a new session. Returns the session file path if persisting.
  std::optional<std::string> newSession(const NewSessionOptions& options = {});

  /// Switch to a different session file.
  void setSessionFile(const std::string& path);

  /// Check if this session is persisted to disk.
  [[nodiscard]] bool isPersisted() const;

  // --------------------------------------------------------------------------
  // Getters
  // --------------------------------------------------------------------------

  [[nodiscard]] const std::string& getCwd() const;
  [[nodiscard]] const std::string& getSessionDir() const;
  [[nodiscard]] const std::string& getSessionId() const;
  [[nodiscard]] const std::optional<std::string>& getSessionFile() const;
  [[nodiscard]] std::optional<std::string> getLeafId() const;
  [[nodiscard]] std::optional<SessionEntry> getLeafEntry() const;
  [[nodiscard]] std::optional<SessionEntry> getEntry(const std::string& id) const;
  [[nodiscard]] std::optional<std::string> getLabel(const std::string& id) const;
  [[nodiscard]] std::optional<SessionHeader> getHeader() const;
  [[nodiscard]] std::optional<std::string> getSessionName() const;

  // --------------------------------------------------------------------------
  // Entry append methods
  // Each appends as a child of the current leaf, advances the leaf, returns entry id.
  // --------------------------------------------------------------------------

  /// Append a message. Does not allow CompactionSummaryMessage or BranchSummaryMessage directly.
  std::string appendMessage(const ChatMessage& message);

  /// Append a thinking level change.
  std::string appendThinkingLevelChange(const std::string& thinkingLevel);

  /// Append a model change.
  std::string appendModelChange(const std::string& provider, const std::string& modelId);

  /// Append a compaction summary.
  std::string appendCompaction(const std::string& summary,
                               const std::string& firstKeptEntryId,
                               int tokensBefore,
                               const std::optional<nlohmann::json>& details = std::nullopt,
                               bool fromHook = false);

  /// Append a custom entry (for extensions — does NOT participate in LLM context).
  std::string appendCustomEntry(const std::string& customType,
                                const std::optional<nlohmann::json>& data = std::nullopt);

  /// Append a custom message entry (for extensions — DOES participate in LLM context).
  std::string appendCustomMessageEntry(const std::string& customType,
                                       const std::string& content,
                                       bool display,
                                       const std::optional<nlohmann::json>& details = std::nullopt);

  /// Append a session info entry (e.g., display name).
  std::string appendSessionInfo(const std::string& name);

  /// Append a branch summary (captures context from an abandoned path).
  std::string appendBranchSummary(const std::string& fromId,
                                   const std::string& summary,
                                   const std::optional<nlohmann::json>& details = std::nullopt,
                                   bool fromHook = false);

  /// Append a label change (set or clear a label on an entry).
  std::string appendLabelChange(const std::string& targetId,
                                 const std::optional<std::string>& label);

  // --------------------------------------------------------------------------
  // Tree traversal
  // --------------------------------------------------------------------------

  /// Get all direct children of an entry.
  std::vector<SessionEntry> getChildren(const std::string& parentId) const;

  /// Walk from entry to root, returning all entries in path order (oldest first).
  /// Pass `std::nullopt` to start from the current leaf.
  [[nodiscard]] std::vector<SessionEntry> getBranch(
      std::optional<std::string> from_entry_id = std::nullopt) const;

  /// Get all session entries (excludes header). Returns a shallow copy.
  std::vector<SessionEntry> getEntries() const;

  /// Get the session as a tree structure with resolved labels.
  std::vector<SessionTreeNode> getTree() const;

  /// Build the session context (what gets sent to the LLM).
  SessionContext buildSessionContext() const;

  // --------------------------------------------------------------------------
  // Branching
  // --------------------------------------------------------------------------

  /// Start a new branch from an earlier entry. Moves the leaf pointer.
  void branch(const std::string& branchFromId);

  /// Reset the leaf pointer to null (before any entries).
  void resetLeaf();

  /// Start a new branch with a summary of the abandoned path.
  std::string branchWithSummary(const std::optional<std::string>& branchFromId,
                                 const std::string& summary,
                                 const std::optional<nlohmann::json>& details = std::nullopt,
                                 bool fromHook = false);

  /// Create a new session file containing only the path from root to the specified leaf.
  std::optional<std::string> createBranchedSession(const std::string& leafId);

  // --------------------------------------------------------------------------
  // Session listing
  // --------------------------------------------------------------------------

  /// List all sessions for a directory.
  static std::vector<SessionInfo> list(const std::string& cwd,
                                        const std::string& sessionDir = "",
                                        const SessionListProgress& onProgress = {});

  /// List all sessions across all project directories.
  static std::vector<SessionInfo> listAll(const SessionListProgress& onProgress = {});

 private:
  // --------------------------------------------------------------------------
  // Internal helpers
  // --------------------------------------------------------------------------

  /// Private constructor — use factory methods.
  SessionManager(const std::string& cwd,
                 const std::string& sessionDir,
                 const std::optional<std::string>& sessionFile,
                 bool persist);

  /// Build the in-memory index from fileEntries.
  void _buildIndex();

  /// Persist an entry (append-only or flush-on-first-assistant).
  void _persist(const SessionEntry& entry);

  /// Rewrite the entire session file.
  void _rewriteFile();

  /// Append an entry to fileEntries, update index, persist.
  void _appendEntry(SessionEntry entry);

  // --------------------------------------------------------------------------
  // State
  // --------------------------------------------------------------------------

  std::string cwd_;
  std::string sessionDir_;
  std::string sessionId_;
  std::optional<std::string> sessionFile_;
  bool persist_ = false;
  bool flushed_ = false;

  /// All file entries (including header).
  std::vector<FileEntry> fileEntries_;

  /// Fast lookup: entry id → entry.
  std::unordered_map<std::string, SessionEntry> byId_;

  /// Label lookup: entry id → label text.
  std::unordered_map<std::string, std::string> labelsById_;

  /// Label timestamp lookup: entry id → timestamp.
  std::unordered_map<std::string, std::string> labelTimestampsById_;

  /// Current leaf pointer.
  std::optional<std::string> leafId_;
};

}  // namespace coding_agent

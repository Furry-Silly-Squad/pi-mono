#include "session_entry.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <random>
#include <sstream>
#include <unordered_set>

#if __has_include(<sys/random.h>)
#include <sys/random.h>
#define CODING_AGENT_HAVE_GETENTROPY 1
#endif

namespace coding_agent {

std::string agent_sessions_root_directory() {
  const char* home = std::getenv("HOME");
  if (home == nullptr) {
    return ".pi/agent/sessions";
  }
  return (std::filesystem::path(home) / ".pi" / "agent" / "sessions").string();
}

std::string session_directory_for_cwd(const std::string& cwd) {
  namespace fs = std::filesystem;
  std::string safePath = "--";
  if (!cwd.empty()) {
    safePath += cwd;
    for (auto& c : safePath) {
      if (c == '/' || c == '\\' || c == ':') {
        c = '-';
      }
    }
  }
  safePath += "--";
  const fs::path sessionDir = fs::path(agent_sessions_root_directory()) / "sessions" / safePath;
  fs::create_directories(sessionDir);
  return sessionDir.string();
}

namespace {

using nlohmann::json;

void seed_mt19937(std::mt19937& rng) {
  std::array<std::uint32_t, 8> data{};
#ifdef CODING_AGENT_HAVE_GETENTROPY
  if (getentropy(reinterpret_cast<unsigned char*>(data.data()),
                 data.size() * sizeof(std::uint32_t)) == 0) {
    std::seed_seq seq(data.begin(), data.end());
    rng.seed(seq);
    return;
  }
#endif
  std::ifstream urandom("/dev/urandom", std::ios::binary);
  if (urandom) {
    urandom.read(reinterpret_cast<char*>(data.data()), data.size() * sizeof(std::uint32_t));
    if (urandom) {
      std::seed_seq seq(data.begin(), data.end());
      rng.seed(seq);
      return;
    }
  }
  std::random_device rd;
  std::seed_seq seq{rd(), rd(), rd(), rd(), rd(), rd(), rd(), rd()};
  rng.seed(seq);
}

std::mt19937& thread_local_rng() {
  static thread_local std::mt19937 rng = [] {
    std::mt19937 r;
    seed_mt19937(r);
    return r;
  }();
  return rng;
}

// Collision-checked 8-hex-char ID generator.
// Tries up to 100 random IDs before falling back to a full UUID.
std::string generateId(std::unordered_set<std::string>& usedIds) {
  std::mt19937& rng = thread_local_rng();
  static const char hex[] = "0123456789abcdef";

  for (int i = 0; i < 100; ++i) {
    std::string id;
    id.reserve(8);
    for (int j = 0; j < 8; ++j) {
      id.push_back(hex[rng() % 16]);
    }
    if (usedIds.find(id) == usedIds.end()) {
      usedIds.insert(id);
      return id;
    }
  }

  // Fallback: full UUID (extremely unlikely to collide)
  std::ostringstream oss;
  for (int i = 0; i < 16; ++i) {
    if (i > 0 && (i == 4 || i == 6 || i == 8 || i == 10)) {
      oss << '-';
    }
    oss << std::hex << std::setfill('0') << std::setw(2) << (rng() & 0xFF);
  }
  return oss.str();
}

std::string generateFreshId() {
  std::unordered_set<std::string> used;
  return generateId(used);
}

std::string generateIdAvoiding(const std::unordered_map<std::string, SessionEntry>& byId) {
  std::unordered_set<std::string> usedIds;
  usedIds.reserve(byId.size());
  for (const auto& kv : byId) {
    usedIds.insert(kv.first);
  }
  return generateId(usedIds);
}

json chatMessageToJson(const ChatMessage& m) {
  json j = {
      {"role", m.role},
      {"content", m.content},
      {"usage_tokens", m.usage_tokens},
  };
  if (m.tool_call_id.has_value()) {
    j["tool_call_id"] = *m.tool_call_id;
  }
  if (!m.tool_calls.empty()) {
    json arr = json::array();
    for (const auto& tc : m.tool_calls) {
      arr.push_back(json{
          {"id", tc.id},
          {"name", tc.name},
          {"arguments_json", tc.arguments_json},
      });
    }
    j["tool_calls"] = std::move(arr);
  }
  return j;
}

json sessionHeaderToJson(const SessionHeader& h) {
  json j = {{"type", h.type},
            {"version", h.version},
            {"id", h.id},
            {"timestamp", h.timestamp},
            {"cwd", h.cwd}};
  if (h.parentSession.has_value()) {
    j["parentSession"] = *h.parentSession;
  }
  return j;
}

json sessionEntryToJson(const SessionEntry& entry) {
  return std::visit(
      [](const auto& e) -> json {
        using T = std::decay_t<decltype(e)>;
        if constexpr (std::is_same_v<T, SessionMessageEntry>) {
          json j = chatMessageToJson(e.message);
          j["type"] = e.type;
          j["id"] = e.id;
          j["parentId"] = e.parentId;
          j["timestamp"] = e.timestamp;
          return j;
        } else if constexpr (std::is_same_v<T, ThinkingLevelChangeEntry>) {
          return {{"type", e.type},
                  {"id", e.id},
                  {"parentId", e.parentId},
                  {"timestamp", e.timestamp},
                  {"thinkingLevel", e.thinkingLevel}};
        } else if constexpr (std::is_same_v<T, ModelChangeEntry>) {
          return {{"type", e.type},
                  {"id", e.id},
                  {"parentId", e.parentId},
                  {"timestamp", e.timestamp},
                  {"provider", e.provider},
                  {"modelId", e.modelId}};
        } else if constexpr (std::is_same_v<T, CompactionEntry>) {
          json j = {{"type", e.type},
                    {"id", e.id},
                    {"parentId", e.parentId},
                    {"timestamp", e.timestamp},
                    {"summary", e.summary},
                    {"firstKeptEntryId", e.firstKeptEntryId},
                    {"tokensBefore", e.tokensBefore},
                    {"fromHook", e.fromHook}};
          if (e.details.has_value()) {
            j["details"] = *e.details;
          }
          return j;
        } else if constexpr (std::is_same_v<T, BranchSummaryEntry>) {
          json j = {{"type", e.type},
                    {"id", e.id},
                    {"parentId", e.parentId},
                    {"timestamp", e.timestamp},
                    {"fromId", e.fromId},
                    {"summary", e.summary},
                    {"fromHook", e.fromHook}};
          if (e.details.has_value()) {
            j["details"] = *e.details;
          }
          return j;
        } else if constexpr (std::is_same_v<T, CustomEntry>) {
          json j = {{"type", e.type},
                    {"id", e.id},
                    {"parentId", e.parentId},
                    {"timestamp", e.timestamp},
                    {"customType", e.customType}};
          if (e.data.has_value()) {
            j["data"] = *e.data;
          }
          return j;
        } else if constexpr (std::is_same_v<T, LabelEntry>) {
          json j = {{"type", e.type},
                    {"id", e.id},
                    {"parentId", e.parentId},
                    {"timestamp", e.timestamp},
                    {"targetId", e.targetId}};
          if (e.label.has_value()) {
            j["label"] = *e.label;
          }
          return j;
        } else if constexpr (std::is_same_v<T, SessionInfoEntry>) {
          json j = {{"type", e.type}, {"id", e.id}, {"parentId", e.parentId}, {"timestamp", e.timestamp}};
          if (e.name.has_value()) {
            j["name"] = *e.name;
          }
          return j;
        } else if constexpr (std::is_same_v<T, CustomMessageEntry>) {
          json j = {{"type", e.type},
                    {"id", e.id},
                    {"parentId", e.parentId},
                    {"timestamp", e.timestamp},
                    {"customType", e.customType},
                    {"content", e.content},
                    {"display", e.display}};
          if (e.details.has_value()) {
            j["details"] = *e.details;
          }
          return j;
        } else {
          return json::object();
        }
      },
      entry);
}

std::string fileEntryToJsonLine(const FileEntry& raw) {
  return std::visit(
      [](const auto& inner) -> std::string {
        using T = std::decay_t<decltype(inner)>;
        if constexpr (std::is_same_v<T, SessionHeader>) {
          return sessionHeaderToJson(inner).dump();
        } else {
          return sessionEntryToJson(inner).dump();
        }
      },
      raw);
}

std::string numericTimestamp() {
  const auto now = std::chrono::system_clock::now();
  const auto ms = std::chrono::time_point_cast<std::chrono::milliseconds>(now);
  return std::to_string(ms.time_since_epoch().count());
}

std::string sessionIdPrefix() {
  const auto now = std::chrono::system_clock::now();
  const auto ms = std::chrono::time_point_cast<std::chrono::milliseconds>(now);
  return std::to_string(ms.time_since_epoch().count()).replace(13, 0, "_");
}

}  // namespace

// ============================================================================
// Migration helpers
// ============================================================================

namespace {

void migrateV1ToV2(std::vector<FileEntry>& entries) {
  std::unordered_set<std::string> ids;
  std::string prevId;

  for (auto& raw : entries) {
    if (std::holds_alternative<SessionHeader>(raw)) {
      auto& header = std::get<SessionHeader>(raw);
      header.version = 2;
      ids.insert(header.id);
      prevId = header.id;
      continue;
    }

    auto& entry = std::get<SessionEntry>(raw);
    std::string id = generateId(ids);
    entry = std::visit(
        [&id, &prevId](auto& e) -> SessionEntry {
          using T = std::decay_t<decltype(e)>;
          if constexpr (std::is_same_v<T, SessionMessageEntry>) {
            e.id = id;
            e.parentId = prevId;
          } else if constexpr (std::is_same_v<T, CompactionEntry>) {
            e.id = id;
            e.parentId = prevId;
            // Convert firstKeptEntryIndex (v1) to firstKeptEntryId (v2)
            // This is a best-effort heuristic since v1 had index-based tracking
          } else {
            e.id = id;
            e.parentId = prevId;
          }
          return e;
        },
        entry);
    prevId = id;
  }
}

void migrateV2ToV3(std::vector<FileEntry>& entries) {
  for (auto& raw : entries) {
    if (std::holds_alternative<SessionHeader>(raw)) {
      auto& header = std::get<SessionHeader>(raw);
      header.version = 3;
      continue;
    }

    auto& entry = std::get<SessionEntry>(raw);
    if (auto* msg = std::get_if<SessionMessageEntry>(&entry)) {
      // No role migration needed in C++ port (no hookMessage concept)
      (void)msg;
    }
  }
}

bool migrateToCurrentVersion(std::vector<FileEntry>& entries) {
  std::optional<int> version;
  for (const auto& raw : entries) {
    if (auto* header = std::get_if<SessionHeader>(&raw)) {
      version = header->version;
      break;
    }
  }

  const int currentVersion = version.value_or(1);
  if (currentVersion >= CURRENT_SESSION_VERSION) {
    return false;
  }

  if (currentVersion < 2) {
    migrateV1ToV2(entries);
  }
  if (currentVersion < 3) {
    migrateV2ToV3(entries);
  }

  return true;
}

// ============================================================================
// File I/O helpers
// ============================================================================

std::optional<std::string> findMostRecentSession(const std::string& sessionDir) {
  namespace fs = std::filesystem;
  if (!fs::exists(sessionDir) || !fs::is_directory(sessionDir)) {
    std::cerr << "[session] findMostRecentSession: session directory does not exist: " << sessionDir << "\n" << std::flush;
    return std::nullopt;
  }

  // Collect all .jsonl files first for debug output
  std::vector<std::pair<std::filesystem::path, std::filesystem::file_time_type>> candidates;
  int totalJsonlFiles = 0;

  for (const auto& entry : fs::directory_iterator(sessionDir)) {
    if (!entry.is_regular_file() || entry.path().extension() != ".jsonl") {
      continue;
    }
    totalJsonlFiles++;
    candidates.push_back({entry.path(), entry.last_write_time()});
  }

  if (totalJsonlFiles == 0) {
    std::cerr << "[session] findMostRecentSession: no .jsonl files found in " << sessionDir << "\n" << std::flush;
    return std::nullopt;
  }

  std::optional<std::pair<std::filesystem::path, std::filesystem::file_time_type>> latest;

  for (const auto& [path, mtime] : candidates) {
    // Validate: first line must be a session header with an id
    std::ifstream check(path);
    if (!check.is_open()) continue;

    std::string firstLine;
    if (!std::getline(check, firstLine) || firstLine.empty()) {
      std::cerr << "[session] findMostRecentSession: skipping (empty), " << path << "\n" << std::flush;
      continue;
    }

    try {
      nlohmann::json header = nlohmann::json::parse(firstLine, nullptr, false);
      if (header.is_discarded()) {
        std::cerr << "[session] findMostRecentSession: skipping (invalid JSON), " << path << "\n" << std::flush;
        continue;
      }
      if (header.value("type", "") != "session") {
        std::cerr << "[session] findMostRecentSession: skipping (type != session), " << path << "\n" << std::flush;
        continue;
      }
      if (!header.contains("id") || !header.at("id").is_string()) {
        std::cerr << "[session] findMostRecentSession: skipping (no id), " << path << "\n" << std::flush;
        continue;
      }
    } catch (const std::exception& e) {
      std::cerr << "[session] findMostRecentSession: skipping (parse error: " << e.what() << "), " << path << "\n" << std::flush;
      continue;
    }

    if (!latest.has_value() || mtime > latest->second) {
      latest = {path, mtime};
    }
  }

  if (latest.has_value()) {
    std::cerr << "[session] findMostRecentSession: selected " << latest->first << "\n" << std::flush;
    return std::make_optional(latest->first.string());
  }

  std::cerr << "[session] findMostRecentSession: no valid session files found in " << sessionDir << "\n" << std::flush;
  return std::nullopt;
}

std::vector<FileEntry> loadEntriesFromFile(const std::string& filePath) {
  std::vector<FileEntry> entries;
  namespace fs = std::filesystem;
  if (!fs::exists(filePath)) return entries;

  std::ifstream input(filePath);
  if (!input.is_open()) return entries;

  std::string line;
  while (std::getline(input, line)) {
    if (line.empty()) continue;
    try {
      nlohmann::json j = nlohmann::json::parse(line, nullptr, false);
      if (j.is_discarded()) continue;

      const std::string type = j.value("type", "");
      if (type == "session") {
        SessionHeader header;
        header.type = "session";
        header.version = j.value("version", 1);
        header.id = j.value("id", "");
        header.timestamp = j.value("timestamp", "");
        header.cwd = j.value("cwd", "");
        if (j.contains("parentSession") && j.at("parentSession").is_string()) {
          header.parentSession = j.at("parentSession").get<std::string>();
        }
        entries.push_back(std::move(header));
      } else {
        // Parse as a SessionEntry
        SessionEntry entry;
        const std::string id = j.value("id", "");
        const std::string parentId = j.value("parentId", j.value("parent_id", ""));
        const std::string timestamp = j.value("timestamp", numericTimestamp());

        if (type == "message") {
          SessionMessageEntry msg;
          msg.type = "message";
          msg.id = id;
          msg.parentId = parentId;
          msg.timestamp = timestamp;
          msg.message.role = j.value("role", "");
          msg.message.content = j.value("content", "");
          if (j.contains("tool_call_id") && j.at("tool_call_id").is_string()) {
            msg.message.tool_call_id = j.at("tool_call_id").get<std::string>();
          }
          if (j.contains("tool_calls") && j.at("tool_calls").is_array()) {
            for (const auto& tc : j.at("tool_calls")) {
              msg.message.tool_calls.push_back(ToolCall{
                  .id = tc.value("id", ""),
                  .name = tc.value("name", ""),
                  .arguments_json = tc.value("arguments_json", "{}"),
              });
            }
          }
          msg.message.usage_tokens = j.value("usage_tokens", 0);
          entry = std::move(msg);
        } else if (type == "thinking_level_change") {
          ThinkingLevelChangeEntry tle;
          tle.type = "thinking_level_change";
          tle.id = id;
          tle.parentId = parentId;
          tle.timestamp = timestamp;
          tle.thinkingLevel = j.value("thinkingLevel", j.value("thinking_level", ""));
          entry = std::move(tle);
        } else if (type == "model_change") {
          ModelChangeEntry mce;
          mce.type = "model_change";
          mce.id = id;
          mce.parentId = parentId;
          mce.timestamp = timestamp;
          mce.provider = j.value("provider", "");
          mce.modelId = j.value("modelId", j.value("model_id", ""));
          entry = std::move(mce);
        } else if (type == "compaction") {
          CompactionEntry ce;
          ce.type = "compaction";
          ce.id = id;
          ce.parentId = parentId;
          ce.timestamp = timestamp;
          ce.summary = j.value("summary", "");
          ce.firstKeptEntryId = j.value("firstKeptEntryId", j.value("first_kept_entry_id", ""));
          ce.tokensBefore = j.value("tokensBefore", j.value("tokens_before", 0));
          if (j.contains("details")) {
            ce.details = j.at("details");
          }
          ce.fromHook = j.value("fromHook", j.value("from_hook", false));
          entry = std::move(ce);
        } else if (type == "branch_summary") {
          BranchSummaryEntry bse;
          bse.type = "branch_summary";
          bse.id = id;
          bse.parentId = parentId;
          bse.timestamp = timestamp;
          bse.fromId = j.value("fromId", j.value("from_id", ""));
          bse.summary = j.value("summary", "");
          if (j.contains("details")) {
            bse.details = j.at("details");
          }
          bse.fromHook = j.value("fromHook", j.value("from_hook", false));
          entry = std::move(bse);
        } else if (type == "custom") {
          CustomEntry ce;
          ce.type = "custom";
          ce.id = id;
          ce.parentId = parentId;
          ce.timestamp = timestamp;
          ce.customType = j.value("customType", j.value("custom_type", ""));
          if (j.contains("data")) {
            ce.data = j.at("data");
          }
          entry = std::move(ce);
        } else if (type == "label") {
          LabelEntry le;
          le.type = "label";
          le.id = id;
          le.parentId = parentId;
          le.timestamp = timestamp;
          le.targetId = j.value("targetId", j.value("target_id", ""));
          if (j.contains("label")) {
            le.label = j.at("label").is_null() ? std::nullopt : std::make_optional(j.at("label").get<std::string>());
          }
          entry = std::move(le);
        } else if (type == "session_info") {
          SessionInfoEntry sie;
          sie.type = "session_info";
          sie.id = id;
          sie.parentId = parentId;
          sie.timestamp = timestamp;
          if (j.contains("name")) {
            sie.name = j.at("name").is_null() ? std::nullopt : std::make_optional(j.at("name").get<std::string>());
          }
          entry = std::move(sie);
        } else if (type == "custom_message") {
          CustomMessageEntry cme;
          cme.type = "custom_message";
          cme.id = id;
          cme.parentId = parentId;
          cme.timestamp = timestamp;
          cme.customType = j.value("customType", j.value("custom_type", ""));
          cme.content = j.value("content", "");
          cme.display = j.value("display", false);
          if (j.contains("details")) {
            cme.details = j.at("details");
          }
          entry = std::move(cme);
        } else {
          // Unknown type — skip
          continue;
        }

        entries.push_back(std::move(entry));
      }
    } catch (...) {
      // Skip malformed lines
    }
  }

  return entries;
}

std::optional<SessionInfo> buildSessionInfo(const std::string& filePath) {
  std::ifstream input(filePath);
  if (!input.is_open()) return std::nullopt;

  std::vector<FileEntry> entries;
  std::string line;
  while (std::getline(input, line)) {
    if (line.empty()) continue;
    try {
      nlohmann::json j = nlohmann::json::parse(line, nullptr, false);
      if (j.is_discarded()) continue;

      const std::string type = j.value("type", "");
      const std::string id = j.value("id", "");
      const std::string parentId = j.value("parentId", j.value("parent_id", ""));
      const std::string timestamp = j.value("timestamp", "");

      if (type == "session") {
        SessionHeader header;
        header.type = "session";
        header.version = j.value("version", 1);
        header.id = id;
        header.timestamp = timestamp;
        header.cwd = j.value("cwd", "");
        if (j.contains("parentSession") && j.at("parentSession").is_string()) {
          header.parentSession = j.at("parentSession").get<std::string>();
        }
        entries.push_back(std::move(header));
      } else {
        SessionEntry entry;
        if (type == "message") {
          SessionMessageEntry msg;
          msg.type = "message";
          msg.id = id;
          msg.parentId = parentId;
          msg.timestamp = timestamp;
          msg.message.role = j.value("role", "");
          msg.message.content = j.value("content", "");
          if (j.contains("tool_call_id") && j.at("tool_call_id").is_string()) {
            msg.message.tool_call_id = j.at("tool_call_id").get<std::string>();
          }
          if (j.contains("tool_calls") && j.at("tool_calls").is_array()) {
            for (const auto& tc : j.at("tool_calls")) {
              msg.message.tool_calls.push_back(ToolCall{
                  .id = tc.value("id", ""),
                  .name = tc.value("name", ""),
                  .arguments_json = tc.value("arguments_json", "{}"),
              });
            }
          }
          msg.message.usage_tokens = j.value("usage_tokens", 0);
          entry = std::move(msg);
        } else if (type == "session_info") {
          SessionInfoEntry sie;
          sie.type = "session_info";
          sie.id = id;
          sie.parentId = parentId;
          sie.timestamp = timestamp;
          if (j.contains("name")) {
            sie.name = j.at("name").is_null() ? std::nullopt : std::make_optional(j.at("name").get<std::string>());
          }
          entry = std::move(sie);
        } else {
          // Skip non-message, non-session_info entries for SessionInfo
          continue;
        }
        entries.push_back(std::move(entry));
      }
    } catch (...) {
      continue;
    }
  }

  if (entries.empty()) return std::nullopt;

  // Validate header
  const SessionHeader* header = nullptr;
  for (const auto& raw : entries) {
    if (auto* h = std::get_if<SessionHeader>(&raw)) {
      header = h;
      break;
    }
  }
  if (!header) return std::nullopt;

  namespace fs = std::filesystem;
  const fs::path p(filePath);
  const auto stats = fs::status(p);

  int messageCount = 0;
  std::string firstMessage;
  std::ostringstream allMessages;
  std::optional<std::string> name;
  std::string lastActivityTimestamp;

  for (const auto& raw : entries) {
    if (std::holds_alternative<SessionHeader>(raw)) continue;
    const auto& sent = std::get<SessionEntry>(raw);
    if (auto* info = std::get_if<SessionInfoEntry>(&sent)) {
      if (info->name.has_value() && !info->name->empty()) {
        name = info->name;
      }
      continue;
    }

    if (auto* msg = std::get_if<SessionMessageEntry>(&sent)) {
      if (msg->message.role != "user" && msg->message.role != "assistant") continue;
      if (msg->message.content.empty()) continue;

      messageCount++;
      allMessages << msg->message.content << " ";
      if (firstMessage.empty() && msg->message.role == "user") {
        firstMessage = msg->message.content;
      }
      if (!msg->timestamp.empty()) {
        lastActivityTimestamp = msg->timestamp;
      }
    }
  }

  const std::string created = header->timestamp;
  std::string modified = lastActivityTimestamp.empty() ? created : lastActivityTimestamp;

  return SessionInfo{
      .path = filePath,
      .id = header->id,
      .cwd = header->cwd,
      .name = name,
      .parentSessionPath = header->parentSession,
      .created = created,
      .modified = modified,
      .messageCount = messageCount,
      .firstMessage = firstMessage.empty() ? "(no messages)" : firstMessage,
      .allMessagesText = allMessages.str(),
  };
}

std::vector<SessionInfo> listSessionsFromDir(const std::string& dir,
                                              const SessionListProgress& onProgress,
                                              int progressOffset,
                                              int progressTotal) {
  std::vector<SessionInfo> sessions;
  namespace fs = std::filesystem;

  if (!fs::exists(dir) || !fs::is_directory(dir)) {
    return sessions;
  }

  std::vector<std::string> files;
  for (const auto& entry : fs::directory_iterator(dir)) {
    if (entry.is_regular_file() && entry.path().extension() == ".jsonl") {
      files.push_back(entry.path().string());
    }
  }

  const int total = progressTotal > 0 ? progressTotal : static_cast<int>(files.size());
  int loaded = 0;

  for (const auto& file : files) {
    if (auto info = buildSessionInfo(file)) {
      sessions.push_back(std::move(*info));
    }
    loaded++;
    if (onProgress) {
      onProgress(progressOffset + loaded, total);
    }
  }

  return sessions;
}

}  // namespace

SessionContext buildSessionContext(const std::vector<SessionEntry>& entries,
                                   const std::optional<std::string>* leafId,
                                   const std::unordered_map<std::string, SessionEntry>& byId);

// ============================================================================
// SessionManager implementation
//
// Threading model: SessionManager is NOT thread-safe. All methods must be
// called from a single thread. The caller (AgentSession) is single-threaded
// in the current port. Concurrent access from multiple threads is not
// supported and would cause data races on fileEntries_, byId_, leafId_, etc.
//
// File locking: SessionManager does not use file locking (flock/fcntl).
// Concurrent processes writing to the same .jsonl file may interleave
// writes and corrupt the file. This matches the TypeScript port's behavior.
// If concurrent access is needed, callers must serialize access externally.
// ============================================================================

SessionManager::SessionManager(const std::string& cwd,
                                const std::string& sessionDir,
                                const std::optional<std::string>& sessionFile,
                                bool persist)
    : cwd_(cwd),
      sessionDir_(sessionDir),
      persist_(persist) {
  if (persist_ && !sessionDir_.empty() && !std::filesystem::exists(sessionDir_)) {
    std::filesystem::create_directories(sessionDir_);
  }

  if (sessionFile.has_value()) {
    setSessionFile(sessionFile.value());
  } else {
    newSession();
  }
}

std::unique_ptr<SessionManager> SessionManager::create(const std::string& cwd,
                                                        const std::string& sessionDir) {
  const std::string dir = sessionDir.empty() ? session_directory_for_cwd(cwd) : sessionDir;
  return std::unique_ptr<SessionManager>(new SessionManager(cwd, dir, std::nullopt, true));
}

std::unique_ptr<SessionManager> SessionManager::open(const std::string& path,
                                                      const std::string& sessionDir,
                                                      const std::string& cwdOverride) {
  const std::vector<FileEntry> entries = loadEntriesFromFile(path);
  std::optional<std::string> cwd;
  for (const auto& raw : entries) {
    if (auto* header = std::get_if<SessionHeader>(&raw)) {
      cwd = header->cwd.empty() ? std::nullopt : std::make_optional(header->cwd);
      break;
    }
  }

  const std::string resolvedCwd = cwdOverride.empty() ? (cwd.value_or(std::string{})) : cwdOverride;
  const std::string dir = sessionDir.empty() ? std::filesystem::path(path).parent_path().string() : sessionDir;
  return std::unique_ptr<SessionManager>(new SessionManager(resolvedCwd, dir, path, true));
}

std::unique_ptr<SessionManager> SessionManager::continueRecent(const std::string& cwd,
                                                                const std::string& sessionDir) {
  const std::string dir = sessionDir.empty() ? session_directory_for_cwd(cwd) : sessionDir;
  std::cerr << "[session] continueRecent: looking for most recent session in: " << dir << "\n" << std::flush;
  const std::optional<std::string> mostRecent = findMostRecentSession(dir);
  if (mostRecent.has_value()) {
    std::cerr << "[session] continueRecent: resuming session: " << mostRecent.value() << "\n" << std::flush;
    return open(mostRecent.value(), dir, cwd);
  }
  std::cerr << "[session] continueRecent: no existing session found, creating new session in: " << dir << "\n" << std::flush;
  return create(cwd, dir);
}

std::unique_ptr<SessionManager> SessionManager::inMemory(const std::string& cwd) {
  return std::unique_ptr<SessionManager>(new SessionManager(cwd, "", std::nullopt, false));
}

std::unique_ptr<SessionManager> SessionManager::forkFrom(const std::string& sourcePath,
                                                          const std::string& targetCwd,
                                                          const std::string& sessionDir) {
  const std::vector<FileEntry> sourceEntries = loadEntriesFromFile(sourcePath);
  if (sourceEntries.empty()) {
    throw std::runtime_error("Cannot fork: source session file is empty or invalid: " + sourcePath);
  }

  bool hasHeader = false;
  for (const auto& raw : sourceEntries) {
    if (std::holds_alternative<SessionHeader>(raw)) {
      hasHeader = true;
      break;
    }
  }
  if (!hasHeader) {
    throw std::runtime_error("Cannot fork: source session has no header: " + sourcePath);
  }

  const std::string dir = sessionDir.empty() ? session_directory_for_cwd(targetCwd) : sessionDir;
  if (!std::filesystem::exists(dir)) {
    std::filesystem::create_directories(dir);
  }

  const std::string newSessionId = sessionIdPrefix() + generateFreshId();
  const std::string timestamp = numericTimestamp();
  const std::string fileTimestamp = timestamp;
  const std::string newSessionFile = (std::filesystem::path(dir) / (fileTimestamp + "_" + newSessionId + ".jsonl")).string();

  // Write new header
  {
    std::ofstream out(newSessionFile, std::ios::app);
    nlohmann::json header{
        {"type", "session"},
        {"version", CURRENT_SESSION_VERSION},
        {"id", newSessionId},
        {"timestamp", timestamp},
        {"cwd", targetCwd},
    };
    out << header.dump() << "\n";
  }

  // Copy all non-header entries
  {
    std::ofstream out(newSessionFile, std::ios::app);
    for (const auto& raw : sourceEntries) {
      if (std::holds_alternative<SessionHeader>(raw)) continue;
      out << fileEntryToJsonLine(raw) << "\n";
    }
  }

  return open(newSessionFile, dir, targetCwd);
}

std::unique_ptr<SessionManager> SessionManager::openBySessionId(const std::string& cwd,
                                                                 const std::string& sessionId,
                                                                 const std::string& sessionDir) {
  namespace fs = std::filesystem;
  const std::string dir = sessionDir.empty() ? session_directory_for_cwd(cwd) : sessionDir;
  if (!fs::exists(dir) || !fs::is_directory(dir)) {
    return nullptr;
  }

  const fs::path legacy = fs::path(dir) / (sessionId + ".jsonl");
  if (fs::exists(legacy)) {
    return open(legacy.string(), sessionDir, cwd);
  }

  try {
    for (const auto& entry : fs::directory_iterator(dir)) {
      if (!entry.is_regular_file() || entry.path().extension() != ".jsonl") {
        continue;
      }
      std::ifstream in(entry.path());
      std::string line;
      if (!std::getline(in, line) || line.empty()) {
        continue;
      }
      const nlohmann::json j = nlohmann::json::parse(line, nullptr, false);
      if (j.is_discarded() || j.value("type", "") != "session") {
        continue;
      }
      if (j.value("id", "") == sessionId) {
        return open(entry.path().string(), sessionDir, cwd);
      }
    }
  } catch (...) {
    return nullptr;
  }
  return nullptr;
}

std::optional<std::string> SessionManager::newSession(const NewSessionOptions& options) {
  sessionId_ = options.id.value_or(sessionIdPrefix() + generateFreshId());
  const std::string timestamp = numericTimestamp();

  SessionHeader header;
  header.type = "session";
  header.version = CURRENT_SESSION_VERSION;
  header.id = sessionId_;
  header.timestamp = timestamp;
  header.cwd = cwd_;
  header.parentSession = options.parentSession;

  fileEntries_ = {std::move(header)};
  byId_.clear();
  labelsById_.clear();
  labelTimestampsById_.clear();
  leafId_ = std::nullopt;
  flushed_ = false;

  if (persist_ && !sessionDir_.empty()) {
    const std::string fileTimestamp = timestamp;
    sessionFile_ = (std::filesystem::path(sessionDir_) / (fileTimestamp + "_" + sessionId_ + ".jsonl")).string();
  }

  return sessionFile_;
}

void SessionManager::setSessionFile(const std::string& path) {
  namespace fs = std::filesystem;
  sessionFile_ = fs::path(path).string();

  if (fs::exists(sessionFile_.value())) {
    std::cerr << "[session] setSessionFile: opening existing session: " << sessionFile_.value() << "\n" << std::flush;
    fileEntries_ = loadEntriesFromFile(sessionFile_.value());

    // If file was empty or corrupted, start fresh
    if (fileEntries_.empty()) {
      std::cerr << "[session] setSessionFile: file was empty or corrupted, starting fresh session\n" << std::flush;
      newSession();
      // Preserve the explicit path from the caller
      sessionFile_ = fs::path(path).string();
      _rewriteFile();
      flushed_ = true;
      return;
    }

    // Extract session ID from header
    for (const auto& raw : fileEntries_) {
      if (auto* header = std::get_if<SessionHeader>(&raw)) {
        sessionId_ = header->id;
        std::cerr << "[session] setSessionFile: loaded session id=" << sessionId_
                  << " version=" << header->version
                  << " cwd=" << header->cwd
                  << " entries=" << fileEntries_.size()
                  << "\n" << std::flush;
        break;
      }
    }

    // Migrate if needed
    if (migrateToCurrentVersion(fileEntries_)) {
      std::cerr << "[session] setSessionFile: migrations applied, rewriting file\n" << std::flush;
      _rewriteFile();
    }

    _buildIndex();
    flushed_ = true;
  } else {
    // File doesn't exist — create new session at this path
    std::cerr << "[session] setSessionFile: file does not exist, creating new session: " << sessionFile_.value() << "\n" << std::flush;
    newSession();
    sessionFile_ = fs::path(path).string();
  }
}

bool SessionManager::isPersisted() const {
  return persist_;
}

const std::string& SessionManager::getCwd() const {
  return cwd_;
}

const std::string& SessionManager::getSessionDir() const {
  return sessionDir_;
}

const std::string& SessionManager::getSessionId() const {
  return sessionId_;
}

const std::optional<std::string>& SessionManager::getSessionFile() const {
  return sessionFile_;
}

std::optional<std::string> SessionManager::getLeafId() const {
  return leafId_;
}

std::optional<SessionEntry> SessionManager::getLeafEntry() const {
  if (!leafId_.has_value()) return std::nullopt;
  auto it = byId_.find(leafId_.value());
  return it != byId_.end() ? std::make_optional(it->second) : std::nullopt;
}

std::optional<SessionEntry> SessionManager::getEntry(const std::string& id) const {
  auto it = byId_.find(id);
  return it != byId_.end() ? std::make_optional(it->second) : std::nullopt;
}

std::optional<std::string> SessionManager::getLabel(const std::string& id) const {
  auto it = labelsById_.find(id);
  return it != labelsById_.end() ? std::make_optional(it->second) : std::nullopt;
}

std::optional<SessionHeader> SessionManager::getHeader() const {
  for (const auto& raw : fileEntries_) {
    if (auto* header = std::get_if<SessionHeader>(&raw)) {
      return *header;
    }
  }
  return std::nullopt;
}

std::optional<std::string> SessionManager::getSessionName() const {
  for (auto it = fileEntries_.rbegin(); it != fileEntries_.rend(); ++it) {
    if (std::holds_alternative<SessionHeader>(*it)) continue;
    const auto& sent = std::get<SessionEntry>(*it);
    if (auto* info = std::get_if<SessionInfoEntry>(&sent)) {
      return info->name.has_value() && !info->name->empty() ? std::make_optional(*info->name) : std::nullopt;
    }
  }
  return std::nullopt;
}

std::string SessionManager::appendMessage(const ChatMessage& message) {
  SessionMessageEntry entry;
  entry.type = "message";
  entry.id = generateIdAvoiding(byId_);
  entry.parentId = leafId_.value_or("");
  entry.timestamp = numericTimestamp();
  entry.message = message;
  entry.message.entry_id = entry.id;
  const std::string newId = entry.id;
  _appendEntry(std::move(entry));
  return newId;
}

std::string SessionManager::appendThinkingLevelChange(const std::string& thinkingLevel) {
  ThinkingLevelChangeEntry entry;
  entry.type = "thinking_level_change";
  entry.id = generateIdAvoiding(byId_);
  entry.parentId = leafId_.value_or("");
  entry.timestamp = numericTimestamp();
  entry.thinkingLevel = thinkingLevel;
  const std::string newId = entry.id;
  _appendEntry(std::move(entry));
  return newId;
}

std::string SessionManager::appendModelChange(const std::string& provider, const std::string& modelId) {
  ModelChangeEntry entry;
  entry.type = "model_change";
  entry.id = generateIdAvoiding(byId_);
  entry.parentId = leafId_.value_or("");
  entry.timestamp = numericTimestamp();
  entry.provider = provider;
  entry.modelId = modelId;
  const std::string newId = entry.id;
  _appendEntry(std::move(entry));
  return newId;
}

std::string SessionManager::appendCompaction(const std::string& summary,
                                              const std::string& firstKeptEntryId,
                                              int tokensBefore,
                                              const std::optional<nlohmann::json>& details,
                                              bool fromHook) {
  CompactionEntry entry;
  entry.type = "compaction";
  entry.id = generateIdAvoiding(byId_);
  entry.parentId = leafId_.value_or("");
  entry.timestamp = numericTimestamp();
  entry.summary = summary;
  entry.firstKeptEntryId = firstKeptEntryId;
  entry.tokensBefore = tokensBefore;
  entry.details = details;
  entry.fromHook = fromHook;
  const std::string newId = entry.id;
  _appendEntry(std::move(entry));
  return newId;
}

std::string SessionManager::appendCustomEntry(const std::string& customType,
                                               const std::optional<nlohmann::json>& data) {
  CustomEntry entry;
  entry.type = "custom";
  entry.id = generateIdAvoiding(byId_);
  entry.parentId = leafId_.value_or("");
  entry.timestamp = numericTimestamp();
  entry.customType = customType;
  entry.data = data;
  const std::string newId = entry.id;
  _appendEntry(std::move(entry));
  return newId;
}

std::string SessionManager::appendCustomMessageEntry(const std::string& customType,
                                                      const std::string& content,
                                                      bool display,
                                                      const std::optional<nlohmann::json>& details) {
  CustomMessageEntry entry;
  entry.type = "custom_message";
  entry.id = generateIdAvoiding(byId_);
  entry.parentId = leafId_.value_or("");
  entry.timestamp = numericTimestamp();
  entry.customType = customType;
  entry.content = content;
  entry.display = display;
  entry.details = details;
  const std::string newId = entry.id;
  _appendEntry(std::move(entry));
  return newId;
}

std::string SessionManager::appendSessionInfo(const std::string& name) {
  SessionInfoEntry entry;
  entry.type = "session_info";
  entry.id = generateIdAvoiding(byId_);
  entry.parentId = leafId_.value_or("");
  entry.timestamp = numericTimestamp();
  entry.name = name.empty() ? std::nullopt : std::make_optional(name);
  const std::string newId = entry.id;
  _appendEntry(std::move(entry));
  return newId;
}

std::string SessionManager::appendBranchSummary(const std::string& fromId,
                                                 const std::string& summary,
                                                 const std::optional<nlohmann::json>& details,
                                                 bool fromHook) {
  BranchSummaryEntry entry;
  entry.type = "branch_summary";
  entry.id = generateIdAvoiding(byId_);
  entry.parentId = leafId_.value_or("");
  entry.timestamp = numericTimestamp();
  entry.fromId = fromId;
  entry.summary = summary;
  entry.details = details;
  entry.fromHook = fromHook;
  const std::string newId = entry.id;
  _appendEntry(std::move(entry));
  return newId;
}

std::string SessionManager::appendLabelChange(const std::string& targetId,
                                               const std::optional<std::string>& label) {
  if (byId_.find(targetId) == byId_.end()) {
    throw std::runtime_error("Entry " + targetId + " not found");
  }

  LabelEntry entry;
  entry.type = "label";
  entry.id = generateIdAvoiding(byId_);
  entry.parentId = leafId_.value_or("");
  entry.timestamp = numericTimestamp();
  entry.targetId = targetId;
  entry.label = label;
  const std::string newId = entry.id;
  const std::string ts = entry.timestamp;
  _appendEntry(std::move(entry));

  if (label.has_value()) {
    labelsById_[targetId] = label.value();
    labelTimestampsById_[targetId] = ts;
  } else {
    labelsById_.erase(targetId);
    labelTimestampsById_.erase(targetId);
  }

  return newId;
}

std::vector<SessionEntry> SessionManager::getChildren(const std::string& parentId) const {
  std::vector<SessionEntry> children;
  for (const auto& raw : fileEntries_) {
    if (std::holds_alternative<SessionHeader>(raw)) continue;
    const auto& entry = std::get<SessionEntry>(raw);
    const std::string* parentIdStr = std::visit(
        [](const auto& e) -> const std::string* { return &e.parentId; },
        entry);
    if (parentIdStr && *parentIdStr == parentId) {
      children.push_back(entry);
    }
  }
  return children;
}

std::vector<SessionEntry> SessionManager::getBranch(std::optional<std::string> from_entry_id) const {
  std::vector<SessionEntry> path;
  const std::string startId =
      from_entry_id.has_value() ? from_entry_id.value() : (leafId_.value_or(""));
  if (startId.empty()) return path;

  auto current = byId_.find(startId);
  while (current != byId_.end()) {
    path.insert(path.begin(), current->second);
    const std::string& parent = std::visit(
        [](const auto& e) -> const std::string& { return e.parentId; },
        current->second);
    if (parent.empty()) break;
    current = byId_.find(parent);
  }
  return path;
}

std::vector<SessionEntry> SessionManager::getEntries() const {
  std::vector<SessionEntry> entries;
  for (const auto& raw : fileEntries_) {
    if (std::holds_alternative<SessionHeader>(raw)) continue;
    entries.push_back(std::get<SessionEntry>(raw));
  }
  return entries;
}

std::vector<SessionTreeNode> SessionManager::getTree() const {
  const std::vector<SessionEntry> entries = getEntries();
  std::unordered_map<std::string, SessionTreeNode> nodeMap;
  std::vector<SessionTreeNode> roots;

  // Create nodes with resolved labels
  for (const auto& entry : entries) {
    const std::string* id = std::visit(
        [](const auto& e) -> const std::string* { return &e.id; },
        entry);
    const std::string label = labelsById_.count(*id) ? labelsById_.at(*id) : "";
    const std::string* labelTimestamp = labelTimestampsById_.count(*id)
                                            ? &labelTimestampsById_.at(*id)
                                            : nullptr;
    nodeMap[*id] = SessionTreeNode{
        .entry = entry,
        .children = {},
        .label = label.empty() ? std::nullopt : std::make_optional(label),
        .labelTimestamp = labelTimestamp ? std::make_optional(*labelTimestamp) : std::nullopt,
    };
  }

  // Build tree
  for (const auto& entry : entries) {
    const std::string* id = std::visit(
        [](const auto& e) -> const std::string* { return &e.id; },
        entry);
    const std::string* parentId = std::visit(
        [](const auto& e) -> const std::string* { return &e.parentId; },
        entry);

    auto it = nodeMap.find(*id);
    if (it == nodeMap.end()) continue;

    SessionTreeNode& node = it->second;

    if (parentId->empty() || *parentId == *id) {
      roots.push_back(std::move(node));
    } else {
      auto parentIt = nodeMap.find(*parentId);
      if (parentIt != nodeMap.end()) {
        parentIt->second.children.push_back(std::move(node));
      } else {
        // Orphan — treat as root
        roots.push_back(std::move(node));
      }
    }
  }

  // Sort children by timestamp (oldest first)
  std::function<void(SessionTreeNode&)> sortChildren;
  sortChildren = [&sortChildren](SessionTreeNode& node) {
    std::sort(node.children.begin(), node.children.end(),
              [](const SessionTreeNode& a, const SessionTreeNode& b) {
                const std::string* tsA = std::visit(
                    [](const auto& e) -> const std::string* { return &e.timestamp; },
                    a.entry);
                const std::string* tsB = std::visit(
                    [](const auto& e) -> const std::string* { return &e.timestamp; },
                    b.entry);
                return *tsA < *tsB;
              });
    for (auto& child : node.children) {
      sortChildren(child);
    }
  };

  for (auto& root : roots) {
    sortChildren(root);
  }

  return roots;
}

SessionContext SessionManager::buildSessionContext() const {
  return ::coding_agent::buildSessionContext(getEntries(), &leafId_, byId_);
}

void SessionManager::branch(const std::string& branchFromId) {
  if (byId_.find(branchFromId) == byId_.end()) {
    throw std::runtime_error("Entry " + branchFromId + " not found");
  }
  leafId_ = branchFromId;
}

void SessionManager::resetLeaf() {
  leafId_ = std::nullopt;
}

std::string SessionManager::branchWithSummary(const std::optional<std::string>& branchFromId,
                                               const std::string& summary,
                                               const std::optional<nlohmann::json>& details,
                                               bool fromHook) {
  if (branchFromId.has_value() && byId_.find(branchFromId.value()) == byId_.end()) {
    throw std::runtime_error("Entry " + branchFromId.value() + " not found");
  }

  leafId_ = branchFromId;
  const std::string fromId = branchFromId.value_or("root");

  BranchSummaryEntry entry;
  entry.type = "branch_summary";
  entry.id = generateIdAvoiding(byId_);
  entry.parentId = branchFromId.value_or("");
  entry.timestamp = numericTimestamp();
  entry.fromId = fromId;
  entry.summary = summary;
  entry.details = details;
  entry.fromHook = fromHook;
  const std::string newId = entry.id;
  _appendEntry(std::move(entry));
  return newId;
}

std::optional<std::string> SessionManager::createBranchedSession(const std::string& leafId) {
  const std::vector<SessionEntry> path = getBranch(leafId);
  if (path.empty()) {
    throw std::runtime_error("Entry " + leafId + " not found");
  }

  // Filter out LabelEntry from path — we'll recreate them
  std::vector<SessionEntry> pathWithoutLabels;
  std::unordered_set<std::string> pathEntryIds;
  for (const auto& entry : path) {
    if (std::holds_alternative<LabelEntry>(entry)) continue;
    pathWithoutLabels.push_back(entry);
    pathEntryIds.insert(std::visit(
        [](const auto& e) -> const std::string& { return e.id; },
        entry));
  }

  const std::string newSessionId = sessionIdPrefix() + generateFreshId();
  const std::string timestamp = numericTimestamp();
  const std::string fileTimestamp = timestamp;
  const std::string newSessionFile = (std::filesystem::path(sessionDir_) / (fileTimestamp + "_" + newSessionId + ".jsonl")).string();

  // Collect labels for entries in the path
  struct LabelInfo {
    std::string targetId;
    std::string label;
    std::string timestamp;
  };
  std::vector<LabelInfo> labelsToWrite;
  for (const auto& [targetId, label] : labelsById_) {
    if (pathEntryIds.count(targetId)) {
      labelsToWrite.push_back({targetId, label, labelTimestampsById_.at(targetId)});
    }
  }

  // Build label entries (chain after last non-label entry on the path).
  std::vector<LabelEntry> labelEntries;
  const std::string lastEntryId = pathWithoutLabels.empty() ? "" : std::visit(
      [](const auto& e) -> const std::string& { return e.id; },
      pathWithoutLabels.back());

  std::unordered_set<std::string> allIds(pathEntryIds.begin(), pathEntryIds.end());
  std::string curParent = lastEntryId;
  for (const auto& li : labelsToWrite) {
    LabelEntry le;
    le.type = "label";
    le.id = generateId(allIds);
    le.parentId = curParent;
    le.timestamp = li.timestamp;
    le.targetId = li.targetId;
    le.label = li.label;
    curParent = le.id;
    labelEntries.push_back(le);
  }

  // Build file entries
  std::vector<FileEntry> newFileEntries;
  {
    SessionHeader header;
    header.type = "session";
    header.version = CURRENT_SESSION_VERSION;
    header.id = newSessionId;
    header.timestamp = timestamp;
    header.cwd = cwd_;
    if (persist_ && sessionFile_.has_value()) {
      header.parentSession = sessionFile_.value();
    }
    newFileEntries.push_back(std::move(header));
  }
  for (const auto& entry : pathWithoutLabels) {
    newFileEntries.push_back(entry);
  }
  for (const auto& le : labelEntries) {
    newFileEntries.push_back(le);
  }

  sessionId_ = newSessionId;
  sessionFile_ = newSessionFile;
  fileEntries_ = std::move(newFileEntries);
  _buildIndex();

  if (persist_) {
    _rewriteFile();
    flushed_ = true;
  }

  return sessionFile_;
}

void SessionManager::_buildIndex() {
  byId_.clear();
  labelsById_.clear();
  labelTimestampsById_.clear();
  leafId_ = std::nullopt;

  for (const auto& raw : fileEntries_) {
    if (std::holds_alternative<SessionHeader>(raw)) continue;
    const auto& entry = std::get<SessionEntry>(raw);
    byId_[std::visit([](const auto& e) -> const std::string& { return e.id; }, entry)] = entry;
    leafId_ = std::visit([](const auto& e) -> const std::string& { return e.id; }, entry);

    if (auto* labelEntry = std::get_if<LabelEntry>(&entry)) {
      if (labelEntry->label.has_value()) {
        labelsById_[labelEntry->targetId] = labelEntry->label.value();
        labelTimestampsById_[labelEntry->targetId] = labelEntry->timestamp;
      } else {
        labelsById_.erase(labelEntry->targetId);
        labelTimestampsById_.erase(labelEntry->targetId);
      }
    }
  }
}

void SessionManager::_persist(const SessionEntry& entry) {
  if (!persist_ || !sessionFile_.has_value()) return;

  // Check if we have any assistant message yet
  bool hasAssistant = false;
  for (const auto& raw : fileEntries_) {
    if (std::holds_alternative<SessionHeader>(raw)) continue;
    const auto& sent = std::get<SessionEntry>(raw);
    if (auto* msg = std::get_if<SessionMessageEntry>(&sent)) {
      if (msg->message.role == "assistant") {
        hasAssistant = true;
        break;
      }
    }
  }

  if (!hasAssistant) {
    // Deferred write — will flush when assistant arrives
    flushed_ = false;
    return;
  }

  if (!flushed_) {
    // Flush all pending entries (including header)
    std::ofstream out(sessionFile_.value(), std::ios::app);
    for (const auto& raw : fileEntries_) {
      out << fileEntryToJsonLine(raw) << "\n";
    }
    flushed_ = true;
  } else {
    // Append single entry
    std::ofstream out(sessionFile_.value(), std::ios::app);
    out << sessionEntryToJson(entry).dump() << "\n";
  }
}

void SessionManager::_rewriteFile() {
  if (!persist_ || !sessionFile_.has_value()) return;

  std::ofstream out(sessionFile_.value(), std::ios::trunc);
  for (const auto& raw : fileEntries_) {
    out << fileEntryToJsonLine(raw) << "\n";
  }
}

void SessionManager::_appendEntry(SessionEntry entry) {
  const std::string id = std::visit([](const auto& e) -> const std::string& { return e.id; }, entry);
  fileEntries_.push_back(std::move(entry));
  byId_[id] = std::get<SessionEntry>(fileEntries_.back());
  leafId_ = id;
  _persist(byId_[id]);
}

std::vector<SessionInfo> SessionManager::list(const std::string& cwd,
                                               const std::string& sessionDir,
                                               const SessionListProgress& onProgress) {
  const std::string dir = sessionDir.empty() ? session_directory_for_cwd(cwd) : sessionDir;
  std::vector<SessionInfo> sessions = listSessionsFromDir(dir, onProgress, 0, 0);
  std::sort(sessions.begin(), sessions.end(),
            [](const SessionInfo& a, const SessionInfo& b) { return a.modified > b.modified; });
  return sessions;
}

std::vector<SessionInfo> SessionManager::listAll(const SessionListProgress& onProgress) {
  const std::string sessionsDir = agent_sessions_root_directory();
  namespace fs = std::filesystem;

  std::vector<SessionInfo> sessions;
  if (!fs::exists(sessionsDir) || !fs::is_directory(sessionsDir)) {
    return sessions;
  }

  // Collect all session files
  struct DirFiles {
    std::string dir;
    std::vector<std::string> files;
  };
  std::vector<DirFiles> dirFiles;
  int totalFiles = 0;

  for (const auto& entry : fs::directory_iterator(sessionsDir)) {
    if (!entry.is_directory()) continue;
    std::vector<std::string> files;
    for (const auto& f : fs::directory_iterator(entry.path())) {
      if (f.is_regular_file() && f.path().extension() == ".jsonl") {
        files.push_back(f.path().string());
      }
    }
    if (!files.empty()) {
      dirFiles.push_back({entry.path().string(), std::move(files)});
      totalFiles += static_cast<int>(files.size());
    }
  }

  // Process all files
  int loaded = 0;
  for (const auto& df : dirFiles) {
    for (const auto& file : df.files) {
      if (auto info = buildSessionInfo(file)) {
        sessions.push_back(std::move(*info));
      }
      loaded++;
      if (onProgress) {
        onProgress(loaded, totalFiles);
      }
    }
  }

  std::sort(sessions.begin(), sessions.end(),
            [](const SessionInfo& a, const SessionInfo& b) { return a.modified > b.modified; });
  return sessions;
}

// ============================================================================
// buildSessionContext — ported from TypeScript
// ============================================================================

namespace {

std::optional<CompactionEntry> findCompaction(const std::vector<SessionEntry>& path) {
  std::optional<CompactionEntry> result;
  for (const auto& entry : path) {
    if (auto* comp = std::get_if<CompactionEntry>(&entry)) {
      result = *comp;
    }
  }
  return result;
}

}  // namespace

SessionContext buildSessionContext(const std::vector<SessionEntry>& entries,
                                    const std::optional<std::string>* leafId,
                                    const std::unordered_map<std::string, SessionEntry>& byId) {
  SessionContext ctx;
  ctx.thinkingLevel = "off";

  // Build uuid index if not available
  std::unordered_map<std::string, SessionEntry> idIndex;
  if (byId.empty()) {
    for (const auto& entry : entries) {
      const std::string& id = std::visit([](const auto& e) -> const std::string& { return e.id; }, entry);
      idIndex[id] = entry;
    }
  } else {
    idIndex = byId;
  }

  // Find leaf
  std::optional<SessionEntry> leaf;
  if (leafId && !leafId->has_value()) {
    // Explicitly null — return no messages
    return ctx;
  }
  if (leafId && leafId->has_value()) {
    auto it = idIndex.find(leafId->value());
    if (it != idIndex.end()) {
      leaf = it->second;
    }
  }
  if (!leaf.has_value()) {
    if (!entries.empty()) {
      leaf = entries.back();
    }
  }
  if (!leaf.has_value()) {
    return ctx;
  }

  // Walk from leaf to root
  std::vector<SessionEntry> path;
  std::optional<std::string> currentId = std::visit(
      [](const auto& e) -> const std::string& { return e.id; },
      leaf.value());
  while (currentId.has_value()) {
    auto it = idIndex.find(currentId.value());
    if (it == idIndex.end()) break;
    path.insert(path.begin(), it->second);
    const std::string& parentId = std::visit(
        [](const auto& e) -> const std::string& { return e.parentId; },
        it->second);
    currentId = parentId.empty() ? std::nullopt : std::make_optional(parentId);
  }

  // Extract settings and find compaction
  for (const auto& entry : path) {
    if (auto* tle = std::get_if<ThinkingLevelChangeEntry>(&entry)) {
      ctx.thinkingLevel = tle->thinkingLevel;
    }
    if (auto* mce = std::get_if<ModelChangeEntry>(&entry)) {
      ctx.model.provider = mce->provider;
      ctx.model.modelId = mce->modelId;
    }
  }

  const std::optional<CompactionEntry> compaction = findCompaction(path);

  // Build messages
  auto appendMessage = [&ctx](const SessionEntry& entry) {
    if (auto* msg = std::get_if<SessionMessageEntry>(&entry)) {
      ctx.messages.push_back(msg->message);
    } else if (auto* cme = std::get_if<CustomMessageEntry>(&entry)) {
      ChatMessage cm;
      cm.role = "assistant";
      cm.content = cme->content;
      ctx.messages.push_back(cm);
    } else if (auto* bs = std::get_if<BranchSummaryEntry>(&entry)) {
      ChatMessage cm;
      cm.role = "assistant";
      cm.content = "Branch handoff from session " + bs->fromId + ":\n" + bs->summary;
      ctx.messages.push_back(cm);
    }
  };

  if (compaction.has_value()) {
    // Emit compaction summary
    ChatMessage cm;
    cm.role = "assistant";
    cm.content = "Compaction summary (tokens before: " + std::to_string(compaction->tokensBefore) + "):\n" + compaction->summary;
    ctx.messages.push_back(cm);

    // Find compaction index in path
    int compactionIdx = -1;
    for (int i = 0; i < static_cast<int>(path.size()); ++i) {
      if (auto* comp = std::get_if<CompactionEntry>(&path[i])) {
        if (comp->id == compaction->id) {
          compactionIdx = i;
          break;
        }
      }
    }

    // Emit kept messages (before compaction, from firstKeptEntryId)
    bool foundFirstKept = false;
    for (int i = 0; i < compactionIdx; ++i) {
      const auto& entry = path[i];
      const std::string& id = std::visit([](const auto& e) -> const std::string& { return e.id; }, entry);
      if (id == compaction->firstKeptEntryId) {
        foundFirstKept = true;
      }
      if (foundFirstKept) {
        appendMessage(entry);
      }
    }

    // Emit messages after compaction
    for (int i = compactionIdx + 1; i < static_cast<int>(path.size()); ++i) {
      appendMessage(path[i]);
    }
  } else {
    // No compaction — emit all messages
    for (const auto& entry : path) {
      appendMessage(entry);
    }
  }

  return ctx;
}

}  // namespace coding_agent

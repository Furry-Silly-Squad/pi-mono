#include "session.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>

#include <nlohmann/json.hpp>

#include "branch_summary.hpp"

namespace coding_agent {
namespace {

using nlohmann::json;

std::string default_session_dir() {
  const char* home = std::getenv("HOME");
  if (home == nullptr) {
    return ".coding-agent/sessions";
  }
  return (std::filesystem::path(home) / ".config" / "coding-agent" / "sessions").string();
}

// Monotonic entry ID counter per session
static int64_t entry_counter = 0;

std::string generate_entry_id() {
  const auto now = std::chrono::system_clock::now();
  const auto ms = std::chrono::time_point_cast<std::chrono::milliseconds>(now);
  const int64_t timestamp = ms.time_since_epoch().count();
  int64_t counter = ++entry_counter;
  return std::to_string(timestamp) + "_" + std::to_string(counter);
}

std::string now_id() {
  const auto now = std::chrono::system_clock::now();
  const auto ms = std::chrono::time_point_cast<std::chrono::milliseconds>(now);
  return std::to_string(ms.time_since_epoch().count());
}

std::optional<std::filesystem::path> latest_session_path(const std::string& session_dir) {
  std::filesystem::file_time_type newest_time;
  std::optional<std::filesystem::path> latest;
  for (const auto& entry : std::filesystem::directory_iterator(session_dir)) {
    if (!entry.is_regular_file() || entry.path().extension() != ".jsonl") {
      continue;
    }
    if (!latest.has_value() || entry.last_write_time() > newest_time) {
      newest_time = entry.last_write_time();
      latest = entry.path();
    }
  }
  return latest;
}

}  // namespace

SessionStore::SessionStore(std::string cwd) : session_dir_(default_session_dir()), session_id_(), session_path_() {
  std::filesystem::create_directories(session_dir_);
  (void)cwd;
}

std::string SessionStore::start_or_resume(const std::optional<std::string>& requested_id, bool force_new) {
  std::optional<std::filesystem::path> previous_latest;
  if (force_new) {
    previous_latest = latest_session_path(session_dir_);
  }

  if (requested_id.has_value()) {
    session_id_ = requested_id.value();
  } else if (force_new) {
    session_id_ = now_id();
  } else {
    std::filesystem::file_time_type newest_time;
    bool found = false;
    for (const auto& entry : std::filesystem::directory_iterator(session_dir_)) {
      if (!entry.is_regular_file() || entry.path().extension() != ".jsonl") {
        continue;
      }
      if (!found || entry.last_write_time() > newest_time) {
        newest_time = entry.last_write_time();
        session_id_ = entry.path().stem().string();
        found = true;
      }
    }
    if (!found) {
      session_id_ = now_id();
    }
  }

  session_path_ = (std::filesystem::path(session_dir_) / (session_id_ + ".jsonl")).string();
  if (!std::filesystem::exists(session_path_)) {
    std::ofstream output(session_path_, std::ios::app);
    output << json({{"type", "session"}, {"id", session_id_}}).dump() << "\n";
  }

  if (force_new && previous_latest.has_value() && previous_latest.value().string() != session_path_) {
    const BranchSummaryEvent event{
        .summary = summarize_branch_session_file(previous_latest.value()),
        .source_session_id = previous_latest.value().stem().string(),
    };
    std::string append_error;
    append_branch_summary(event, append_error);
  }
  return session_id_;
}

bool SessionStore::append(const ChatMessage& message, std::string& error) {
  try {
    std::ofstream output(session_path_, std::ios::app);
    json row{
        {"type", "message"},
        {"role", message.role},
        {"content", message.content},
    };
    if (message.entry_id.has_value()) {
      row["id"] = message.entry_id.value();
    }
    if (message.tool_call_id.has_value()) {
      row["tool_call_id"] = message.tool_call_id.value();
    }
    if (!message.tool_calls.empty()) {
      row["tool_calls"] = json::array();
      for (const auto& call : message.tool_calls) {
        row["tool_calls"].push_back(
            {{"id", call.id}, {"name", call.name}, {"arguments_json", call.arguments_json}}
        );
      }
    }
    if (message.usage_tokens > 0) {
      row["usage_tokens"] = message.usage_tokens;
    }
    output << row.dump() << "\n";
    return true;
  } catch (const std::exception& ex) {
    error = ex.what();
    return false;
  }
}

bool SessionStore::append_compaction(const CompactionEvent& event, std::string& error) {
  try {
    std::ofstream output(session_path_, std::ios::app);
    json row{
        {"type", "compaction"},
        {"tokens_before", event.tokens_before},
        {"tokens_after", event.tokens_after},
        {"summary", event.summary},
    };
    if (event.first_kept_index >= 0) {
      row["first_kept_index"] = event.first_kept_index;
    }
    if (event.first_kept_entry_id.has_value()) {
      row["first_kept_entry_id"] = event.first_kept_entry_id.value();
    }
    output << row.dump() << "\n";
    return true;
  } catch (const std::exception& ex) {
    error = ex.what();
    return false;
  }
}

bool SessionStore::append_branch_summary(const BranchSummaryEvent& event, std::string& error) {
  try {
    std::ofstream output(session_path_, std::ios::app);
    json row{
        {"type", "branch_summary"},
        {"summary", event.summary},
        {"source_session_id", event.source_session_id},
    };
    output << row.dump() << "\n";
    return true;
  } catch (const std::exception& ex) {
    error = ex.what();
    return false;
  }
}

std::vector<ChatMessage> SessionStore::load_messages(std::string& error) {
  std::vector<ChatMessage> out;
  try {
    std::ifstream input(session_path_);
    std::string line;
    while (std::getline(input, line)) {
      if (line.empty()) {
        continue;
      }
      const auto row = json::parse(line);
      if (row.value("type", "") != "message") {
        if (row.value("type", "") == "compaction") {
          const std::string summary = row.value("summary", "");
          const int tokens_before = row.value("tokens_before", 0);
          ChatMessage message{
              .role = "assistant",
              .content = "Compaction summary (tokens before: " + std::to_string(tokens_before) + "):\n" + summary,
              .tool_call_id = std::nullopt,
              .tool_calls = {},
          };
          out.push_back(std::move(message));
          if (row.contains("first_kept_entry_id")) {
            compaction_first_kept_entry_ids_.push_back(row.at("first_kept_entry_id").get<std::string>());
          }
        } else if (row.value("type", "") == "branch_summary") {
          const std::string summary = row.value("summary", "");
          const std::string source = row.value("source_session_id", "");
          ChatMessage message{
              .role = "assistant",
              .content = "Branch handoff from session " + source + ":\n" + summary,
              .tool_call_id = std::nullopt,
              .tool_calls = {},
          };
          out.push_back(std::move(message));
        }
        continue;
      }
      ChatMessage message{
          .role = row.value("role", ""),
          .content = row.value("content", ""),
          .tool_call_id = std::nullopt,
          .tool_calls = {},
      };
      if (row.contains("tool_call_id")) {
        message.tool_call_id = row.at("tool_call_id").get<std::string>();
      }
      if (row.contains("id")) {
        message.entry_id = row.at("id").get<std::string>();
      }
      out.push_back(std::move(message));
    }
  } catch (const std::exception& ex) {
    error = ex.what();
  }
  return out;
}

std::string SessionStore::assign_entry_id() {
  std::string id = generate_entry_id();
  return id;
}

std::optional<std::string> SessionStore::get_last_compaction_first_kept_entry_id() const {
  if (compaction_first_kept_entry_ids_.empty()) {
    return std::nullopt;
  }
  return compaction_first_kept_entry_ids_.back();
}

int SessionStore::get_compaction_count() const {
  return compaction_count_;
}

const CompactionEvent& SessionStore::get_last_compaction_event() const {
  return last_compaction_event_;
}

void SessionStore::record_compaction(const CompactionEvent& event) {
  compaction_count_++;
  last_compaction_event_ = event;
}

std::string SessionStore::get_session_id() const {
  return session_id_;
}

}  // namespace coding_agent

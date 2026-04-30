#include "session.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>

#include <nlohmann/json.hpp>

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

std::string now_id() {
  const auto now = std::chrono::system_clock::now();
  const auto ms = std::chrono::time_point_cast<std::chrono::milliseconds>(now);
  return std::to_string(ms.time_since_epoch().count());
}

}  // namespace

SessionStore::SessionStore(std::string cwd) : session_dir_(default_session_dir()), session_id_(), session_path_() {
  std::filesystem::create_directories(session_dir_);
  (void)cwd;
}

std::string SessionStore::start_or_resume(const std::optional<std::string>& requested_id, bool force_new) {
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
    output << row.dump() << "\n";
    return true;
  } catch (const std::exception& ex) {
    error = ex.what();
    return false;
  }
}

std::vector<ChatMessage> SessionStore::load_messages(std::string& error) const {
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
      out.push_back(std::move(message));
    }
  } catch (const std::exception& ex) {
    error = ex.what();
  }
  return out;
}

}  // namespace coding_agent

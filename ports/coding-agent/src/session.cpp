#include "session.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <unordered_set>

#include <nlohmann/json.hpp>

#include "file_ops.hpp"

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

std::string synthetic_id_for_line(int line_index) {
  return "legacy_" + std::to_string(line_index);
}

CompactionEvent parse_compaction_row(const json& row, const std::string& row_id) {
  CompactionEvent event{};
  event.tokens_before = row.value("tokens_before", 0);
  event.tokens_after = row.value("tokens_after", 0);
  event.summary = row.value("summary", "");
  event.first_kept_index = row.value("first_kept_index", -1);
  event.first_kept_entry_id = row.value("first_kept_entry_id", "");
  if (row.contains("read_files") && row.at("read_files").is_array()) {
    for (const auto& path : row.at("read_files")) {
      if (path.is_string()) {
        event.read_files.push_back(path.get<std::string>());
      }
    }
  }
  if (row.contains("modified_files") && row.at("modified_files").is_array()) {
    for (const auto& path : row.at("modified_files")) {
      if (path.is_string()) {
        event.modified_files.push_back(path.get<std::string>());
      }
    }
  }
  (void)row_id;
  return event;
}

}  // namespace

std::optional<std::filesystem::path> latest_session_path_in_dir(const std::string& session_dir) {
  namespace fs = std::filesystem;
  if (!fs::exists(session_dir) || !fs::is_directory(session_dir)) {
    return std::nullopt;
  }
  fs::file_time_type newest_time;
  std::optional<fs::path> latest;
  for (const auto& entry : fs::directory_iterator(session_dir)) {
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

std::vector<std::string> SessionGraph::get_branch(const std::string& entry_id) const {
  std::vector<std::string> rev;
  std::string cur = entry_id;
  std::unordered_set<std::string> guard;
  while (!cur.empty()) {
    if (guard.count(cur) != 0U) {
      break;
    }
    guard.insert(cur);
    rev.push_back(cur);
    auto it = nodes.find(cur);
    if (it == nodes.end()) {
      break;
    }
    cur = it->second.parent_id;
  }
  std::reverse(rev.begin(), rev.end());
  return rev;
}

std::string SessionGraph::find_common_ancestor(const std::string& id_a, const std::string& id_b) const {
  if (id_a.empty() || id_b.empty()) {
    return "";
  }
  const auto path_a = get_branch(id_a);
  std::unordered_set<std::string> set_a(path_a.begin(), path_a.end());
  const auto path_b = get_branch(id_b);
  for (auto it = path_b.rbegin(); it != path_b.rend(); ++it) {
    if (set_a.count(*it) != 0U) {
      return *it;
    }
  }
  return "";
}

const SessionNode* SessionGraph::get_node(const std::string& id) const {
  auto it = nodes.find(id);
  if (it == nodes.end()) {
    return nullptr;
  }
  return &it->second;
}

bool load_session_graph(const std::string& path, SessionGraph& out, std::string& error) {
  out.nodes.clear();
  out.leaf_id.clear();
  try {
    std::ifstream input(path);
    if (!input.is_open()) {
      error = "cannot open session file";
      return false;
    }
    std::string line;
    int line_index = 0;
    std::string prev_row_id;
    std::string last_id_any;
    while (std::getline(input, line)) {
      ++line_index;
      if (line.empty()) {
        continue;
      }
      const auto row = json::parse(line, nullptr, false);
      if (row.is_discarded()) {
        continue;
      }
      const std::string type = row.value("type", "");

      std::string row_id;
      if (row.contains("id") && row.at("id").is_string()) {
        row_id = row.at("id").get<std::string>();
      } else {
        row_id = synthetic_id_for_line(line_index);
      }

      std::string parent_id;
      if (row.contains("parent_id") && row.at("parent_id").is_string()) {
        parent_id = row.at("parent_id").get<std::string>();
      } else if (!prev_row_id.empty()) {
        parent_id = prev_row_id;
      }

      SessionNode node;
      node.id = row_id;
      node.parent_id = parent_id;

      if (type == "session") {
        node.kind = SessionRowKind::SessionHeader;
      } else if (type == "message") {
        node.kind = SessionRowKind::Message;
        node.message.role = row.value("role", "");
        node.message.content = row.value("content", "");
        if (row.contains("tool_call_id")) {
          node.message.tool_call_id = row.at("tool_call_id").get<std::string>();
        }
        if (row.contains("tool_calls") && row.at("tool_calls").is_array()) {
          for (const auto& tc : row.at("tool_calls")) {
            node.message.tool_calls.push_back(
                ToolCall{
                    .id = tc.value("id", ""),
                    .name = tc.value("name", ""),
                    .arguments_json = tc.value("arguments_json", "{}"),
                }
            );
          }
        }
        node.message.usage_tokens = row.value("usage_tokens", 0);
        node.message.entry_id = row_id;
      } else if (type == "compaction") {
        node.kind = SessionRowKind::Compaction;
        node.compaction = parse_compaction_row(row, row_id);
      } else if (type == "branch_summary") {
        node.kind = SessionRowKind::BranchSummary;
        SessionNode::BranchRowData br;
        br.summary = row.value("summary", "");
        br.source_session_id = row.value("source_session_id", "");
        br.handoff_source_leaf_id = row.value("handoff_source_leaf_id", "");
        if (row.contains("read_files") && row.at("read_files").is_array()) {
          for (const auto& p : row.at("read_files")) {
            if (p.is_string()) {
              br.read_files.push_back(p.get<std::string>());
            }
          }
        }
        if (row.contains("modified_files") && row.at("modified_files").is_array()) {
          for (const auto& p : row.at("modified_files")) {
            if (p.is_string()) {
              br.modified_files.push_back(p.get<std::string>());
            }
          }
        }
        node.branch = std::move(br);
      } else if (type == "compaction_skipped") {
        node.kind = SessionRowKind::CompactionSkipped;
      } else {
        node.kind = SessionRowKind::Message;
      }

      out.nodes[row_id] = std::move(node);
      prev_row_id = row_id;
      last_id_any = row_id;
    }
    out.leaf_id = last_id_any;
    return true;
  } catch (const std::exception& ex) {
    error = ex.what();
    return false;
  }
}

SessionStore::SessionStore(std::string cwd, std::optional<std::string> session_dir_override)
    : session_dir_(
          (session_dir_override.has_value() && !session_dir_override->empty())
              ? std::move(*session_dir_override)
              : default_session_dir()
      ),
      session_id_(),
      session_path_() {
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
    json session_row{
        {"type", "session"},
        {"id", session_id_},
        {"parent_id", ""},
    };
    output << session_row.dump() << "\n";
    graph_.nodes.clear();
    SessionNode root{};
    root.id = session_id_;
    root.parent_id = "";
    root.kind = SessionRowKind::SessionHeader;
    graph_.nodes[session_id_] = std::move(root);
    graph_.leaf_id = session_id_;
    last_written_entry_id_ = session_id_;
  } else {
    std::string graph_error;
    if (!load_session_graph(session_path_, graph_, graph_error)) {
      graph_.nodes.clear();
      graph_.leaf_id.clear();
      last_written_entry_id_.clear();
    } else {
      last_written_entry_id_ = graph_.leaf_id;
    }
  }
  return session_id_;
}

bool SessionStore::append(const ChatMessage& message, std::string& error) {
  try {
    std::string row_id = message.entry_id.has_value() ? message.entry_id.value() : generate_entry_id();
    std::ofstream output(session_path_, std::ios::app);
    json row{
        {"type", "message"},
        {"role", message.role},
        {"content", message.content},
        {"id", row_id},
        {"parent_id", last_written_entry_id_},
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
    if (message.usage_tokens > 0) {
      row["usage_tokens"] = message.usage_tokens;
    }
    output << row.dump() << "\n";

    SessionNode node{};
    node.id = row_id;
    node.parent_id = last_written_entry_id_;
    node.kind = SessionRowKind::Message;
    node.message = message;
    node.message.entry_id = row_id;
    graph_.nodes[row_id] = std::move(node);
    last_written_entry_id_ = row_id;
    graph_.leaf_id = row_id;
    return true;
  } catch (const std::exception& ex) {
    error = ex.what();
    return false;
  }
}

bool SessionStore::append_compaction(const CompactionEvent& event, std::string& error) {
  try {
    const std::string row_id = generate_entry_id();
    std::ofstream output(session_path_, std::ios::app);
    json row{
        {"type", "compaction"},
        {"id", row_id},
        {"parent_id", last_written_entry_id_},
        {"tokens_before", event.tokens_before},
        {"tokens_after", event.tokens_after},
        {"summary", event.summary},
    };
    if (event.first_kept_index >= 0) {
      row["first_kept_index"] = event.first_kept_index;
    }
    if (!event.first_kept_entry_id.empty()) {
      row["first_kept_entry_id"] = event.first_kept_entry_id;
    }
    row["read_files"] = event.read_files;
    row["modified_files"] = event.modified_files;
    output << row.dump() << "\n";

    SessionNode node{};
    node.id = row_id;
    node.parent_id = last_written_entry_id_;
    node.kind = SessionRowKind::Compaction;
    node.compaction = event;
    graph_.nodes[row_id] = std::move(node);
    last_written_entry_id_ = row_id;
    graph_.leaf_id = row_id;
    return true;
  } catch (const std::exception& ex) {
    error = ex.what();
    return false;
  }
}

bool SessionStore::append_branch_summary(const BranchSummaryEvent& event, std::string& error) {
  try {
    const std::string row_id = generate_entry_id();
    std::ofstream output(session_path_, std::ios::app);
    json row{
        {"type", "branch_summary"},
        {"id", row_id},
        {"parent_id", last_written_entry_id_},
        {"summary", event.summary},
        {"source_session_id", event.source_session_id},
        {"read_files", event.read_files},
        {"modified_files", event.modified_files},
    };
    if (!event.handoff_source_leaf_id.empty()) {
      row["handoff_source_leaf_id"] = event.handoff_source_leaf_id;
    }
    output << row.dump() << "\n";

    SessionNode node{};
    node.id = row_id;
    node.parent_id = last_written_entry_id_;
    node.kind = SessionRowKind::BranchSummary;
    SessionNode::BranchRowData br{
        .summary = event.summary,
        .source_session_id = event.source_session_id,
        .handoff_source_leaf_id = event.handoff_source_leaf_id,
        .read_files = event.read_files,
        .modified_files = event.modified_files,
    };
    node.branch = std::move(br);
    graph_.nodes[row_id] = std::move(node);
    last_written_entry_id_ = row_id;
    graph_.leaf_id = row_id;
    return true;
  } catch (const std::exception& ex) {
    error = ex.what();
    return false;
  }
}

std::vector<ChatMessage> SessionStore::load_messages(std::string& error) {
  std::vector<ChatMessage> out;
  compaction_first_kept_entry_ids_.clear();
  last_compaction_file_ops_ = FileOps{};
  std::string graph_err;
  if (!load_session_graph(session_path_, graph_, graph_err)) {
    error = graph_err;
    return out;
  }
  last_written_entry_id_ = graph_.leaf_id;

  std::unordered_set<std::string> leaf_path_ids;
  if (!graph_.leaf_id.empty()) {
    const auto path_ids = graph_.get_branch(graph_.leaf_id);
    leaf_path_ids.insert(path_ids.begin(), path_ids.end());
  }

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
          FileOps loaded_file_ops;
          if (row.contains("read_files") && row.at("read_files").is_array()) {
            for (const auto& path : row.at("read_files")) {
              if (path.is_string()) {
                loaded_file_ops.read_files.insert(path.get<std::string>());
              }
            }
          }
          if (row.contains("modified_files") && row.at("modified_files").is_array()) {
            for (const auto& path : row.at("modified_files")) {
              if (path.is_string()) {
                loaded_file_ops.modified_files.insert(path.get<std::string>());
              }
            }
          }
          last_compaction_file_ops_ = std::move(loaded_file_ops);
        } else if (row.value("type", "") == "branch_summary") {
          const std::string handoff = row.value("handoff_source_leaf_id", "");
          if (!handoff.empty() && leaf_path_ids.count(handoff) != 0U) {
            continue;
          }
          const std::string summary = row.value("summary", "");
          const std::string source = row.value("source_session_id", "");
          ChatMessage message{
              .role = "assistant",
              .content = "Branch handoff from session " + source + ":\n" + summary,
              .tool_call_id = std::nullopt,
              .tool_calls = {},
          };
          out.push_back(std::move(message));
        } else if (row.value("type", "") == "compaction_skipped") {
          const std::string reason = row.value("reason", "unknown");
          const int tokens_before = row.value("tokens_before", 0);
          compaction_skipped_events_.push_back(
              CompactionSkippedEvent{
                  .reason = reason,
                  .tokens_before = tokens_before,
              }
          );
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
      if (row.contains("tool_calls") && row.at("tool_calls").is_array()) {
        for (const auto& tc : row.at("tool_calls")) {
          message.tool_calls.push_back(
              ToolCall{
                  .id = tc.value("id", ""),
                  .name = tc.value("name", ""),
                  .arguments_json = tc.value("arguments_json", "{}"),
              }
          );
        }
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

const FileOps& SessionStore::get_last_compaction_file_ops() const {
  return last_compaction_file_ops_;
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
  last_compaction_file_ops_.read_files =
      std::unordered_set<std::string>(event.read_files.begin(), event.read_files.end());
  last_compaction_file_ops_.modified_files =
      std::unordered_set<std::string>(event.modified_files.begin(), event.modified_files.end());
}

std::string SessionStore::get_session_id() const {
  return session_id_;
}

const std::string& SessionStore::get_session_path() const {
  return session_path_;
}

const std::string& SessionStore::session_dir() const {
  return session_dir_;
}

const SessionGraph& SessionStore::graph() const {
  return graph_;
}

std::vector<std::string> SessionStore::get_branch(const std::string& entry_id) const {
  return graph_.get_branch(entry_id);
}

std::string SessionStore::find_common_ancestor(const std::string& id_a, const std::string& id_b) const {
  return graph_.find_common_ancestor(id_a, id_b);
}

}  // namespace coding_agent

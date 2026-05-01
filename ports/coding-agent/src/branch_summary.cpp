#include "branch_summary.hpp"

#include <algorithm>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "compaction.hpp"
#include "file_ops.hpp"
#include <nlohmann/json.hpp>

namespace coding_agent {
namespace {

using nlohmann::json;

constexpr const char* kSummarizationSystemPrompt =
    "You are a context summarization assistant. Your task is to read a conversation between a user and an AI "
    "coding assistant, then produce a structured summary following the exact format specified.\n"
    "\n"
    "Do NOT continue the conversation. Do NOT respond to any questions in the conversation. ONLY output the "
    "structured summary.";

constexpr const char* kBranchSummaryPrompt =
    "Create a structured summary of this conversation branch for context when returning later.\n"
    "\n"
    "Use this EXACT format:\n"
    "\n"
    "## Goal\n"
    "[What was the user trying to accomplish in this branch?]\n"
    "\n"
    "## Constraints & Preferences\n"
    "- [Any constraints, preferences, or requirements mentioned]\n"
    "- [Or \"(none)\" if none were mentioned]\n"
    "\n"
    "## Progress\n"
    "### Done\n"
    "- [x] [Completed tasks/changes]\n"
    "\n"
    "### In Progress\n"
    "- [ ] [Work that was started but not finished]\n"
    "\n"
    "### Blocked\n"
    "- [Issues preventing progress, if any]\n"
    "\n"
    "## Key Decisions\n"
    "- **[Decision]**: [Brief rationale]\n"
    "\n"
    "## Next Steps\n"
    "1. [What should happen next to continue this work]\n"
    "\n"
    "Keep each section concise. Preserve exact file paths, function names, and error messages.";

constexpr const char* kBranchPreamble =
    "The user explored a different conversation branch before returning here.\n"
    "Summary of that exploration:\n"
    "\n";

int estimate_chat_tokens(const ChatMessage& message) {
  if (message.role == "assistant" && message.usage_tokens > 0) {
    return message.usage_tokens;
  }
  int total = approx_tokens(message.content);
  for (const auto& call : message.tool_calls) {
    total += approx_tokens(call.arguments_json);
  }
  return total;
}

ChatMessage entry_to_chat_message(const SessionNode& node) {
  switch (node.kind) {
    case SessionRowKind::Message:
      return node.message;
    case SessionRowKind::Compaction:
      if (!node.compaction.has_value()) {
        return ChatMessage{};
      }
      {
        const CompactionEvent& ev = node.compaction.value();
        return ChatMessage{
            .role = "assistant",
            .content = "Compaction summary (tokens before: " + std::to_string(ev.tokens_before) + "):\n" + ev.summary,
            .tool_call_id = std::nullopt,
            .tool_calls = {},
            .entry_id = std::nullopt,
            .usage_tokens = 0,
        };
      }
    case SessionRowKind::BranchSummary:
      if (!node.branch.has_value()) {
        return ChatMessage{};
      }
      return ChatMessage{
          .role = "assistant",
          .content = node.branch->summary,
          .tool_call_id = std::nullopt,
          .tool_calls = {},
          .entry_id = std::nullopt,
          .usage_tokens = 0,
      };
    default:
      return ChatMessage{};
  }
}

std::string serialize_conversation(const std::vector<ChatMessage>& messages) {
  std::ostringstream out;
  for (const auto& m : messages) {
    if (m.role == "user") {
      out << "[User]: " << m.content << "\n\n";
    } else if (m.role == "assistant") {
      out << "[Assistant]: " << m.content << "\n";
      if (!m.tool_calls.empty()) {
        out << "[Assistant tool calls]: ";
        for (size_t i = 0; i < m.tool_calls.size(); ++i) {
          if (i > 0) {
            out << "; ";
          }
          out << m.tool_calls[i].name << "(" << m.tool_calls[i].arguments_json << ")";
        }
        out << "\n";
      }
      out << "\n";
    }
  }
  return out.str();
}

void merge_row_file_ops(FileOps& dst, const SessionNode& node) {
  if (node.kind == SessionRowKind::Compaction && node.compaction.has_value()) {
    for (const auto& p : node.compaction->read_files) {
      dst.read_files.insert(p);
    }
    for (const auto& p : node.compaction->modified_files) {
      dst.modified_files.insert(p);
    }
  }
  if (node.kind == SessionRowKind::BranchSummary && node.branch.has_value()) {
    for (const auto& p : node.branch->read_files) {
      dst.read_files.insert(p);
    }
    for (const auto& p : node.branch->modified_files) {
      dst.modified_files.insert(p);
    }
  }
}

BranchSummaryData fallback_snippet_from_entries(const std::vector<SessionNode>& entries) {
  std::vector<ChatMessage> linear;
  for (const auto& node : entries) {
    if (node.kind == SessionRowKind::Message) {
      linear.push_back(node.message);
    }
  }
  std::vector<std::string> snippets;
  for (int i = static_cast<int>(linear.size()) - 1; i >= 0 && snippets.size() < 6; --i) {
    const auto& message = linear[static_cast<size_t>(i)];
    const std::string role = message.role;
    const std::string raw_content = message.content;
    if (role != "user" && role != "assistant") {
      continue;
    }
    std::string content = raw_content;
    if (content.size() > 180) {
      content = content.substr(0, 180) + "...";
    }
    snippets.push_back("- " + role + ": " + content);
  }
  std::reverse(snippets.begin(), snippets.end());

  FileOps file_ops{};
  for (const auto& node : entries) {
    merge_row_file_ops(file_ops, node);
    if (node.kind == SessionRowKind::Message) {
      merge_file_ops(file_ops, extract_file_ops_from_messages({node.message}));
    }
  }

  const auto read_files = sorted_file_list(file_ops.read_files);
  const auto modified_files = sorted_file_list(file_ops.modified_files);

  if (snippets.empty()) {
    return BranchSummaryData{
        .summary = "Previous session had no user/assistant messages to summarize.",
        .read_files = read_files,
        .modified_files = modified_files,
    };
  }

  std::string summary = std::string(kBranchPreamble) + "Recent context from previous session:\n";
  for (const auto& snippet : snippets) {
    summary += snippet + "\n";
  }
  summary += build_file_ops_footer(file_ops);
  return BranchSummaryData{
      .summary = summary,
      .read_files = read_files,
      .modified_files = modified_files,
  };
}

}  // namespace

BranchSummaryData summarize_branch_session_file(const std::filesystem::path& session_path) {
  std::ifstream input(session_path);
  std::vector<std::string> lines;
  std::string line;
  while (std::getline(input, line)) {
    if (!line.empty()) {
      lines.push_back(line);
    }
  }

  std::vector<ChatMessage> messages;
  for (const auto& row_line : lines) {
    const auto row = json::parse(row_line, nullptr, false);
    if (row.is_discarded() || row.value("type", "") != "message") {
      continue;
    }
    ChatMessage message{
        .role = row.value("role", ""),
        .content = row.value("content", ""),
        .tool_call_id = std::nullopt,
        .tool_calls = {},
    };
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
    messages.push_back(std::move(message));
  }

  std::vector<std::string> snippets;
  for (int i = static_cast<int>(messages.size()) - 1; i >= 0 && snippets.size() < 6; --i) {
    const auto& message = messages[static_cast<size_t>(i)];
    const std::string role = message.role;
    const std::string raw_content = message.content;
    if (role != "user" && role != "assistant") {
      continue;
    }
    std::string content = raw_content;
    if (content.size() > 180) {
      content = content.substr(0, 180) + "...";
    }
    snippets.push_back("- " + role + ": " + content);
  }
  std::reverse(snippets.begin(), snippets.end());

  const FileOps file_ops = extract_file_ops_from_messages(messages);
  const auto read_files = sorted_file_list(file_ops.read_files);
  const auto modified_files = sorted_file_list(file_ops.modified_files);

  if (snippets.empty()) {
    return BranchSummaryData{
        .summary = "Previous session had no user/assistant messages to summarize.",
        .read_files = read_files,
        .modified_files = modified_files,
    };
  }

  std::string summary = std::string(kBranchPreamble) + "Recent context from previous session:\n";
  for (const auto& snippet : snippets) {
    summary += snippet + "\n";
  }
  summary += build_file_ops_footer(file_ops);
  return BranchSummaryData{
      .summary = summary,
      .read_files = read_files,
      .modified_files = modified_files,
  };
}

std::vector<SessionNode> collect_entries_for_branch_summary(
    const SessionGraph& graph,
    const std::string& old_leaf_id,
    const std::string& target_id
) {
  std::vector<SessionNode> out;
  if (old_leaf_id.empty()) {
    return out;
  }

  if (target_id.empty()) {
    std::string cur = old_leaf_id;
    while (!cur.empty()) {
      auto it = graph.nodes.find(cur);
      if (it == graph.nodes.end()) {
        break;
      }
      const SessionNode& node = it->second;
      if (node.kind == SessionRowKind::SessionHeader) {
        break;
      }
      out.push_back(node);
      cur = node.parent_id;
    }
    std::reverse(out.begin(), out.end());
    return out;
  }

  const std::string common = graph.find_common_ancestor(old_leaf_id, target_id);
  std::string cur = old_leaf_id;
  while (!cur.empty() && cur != common) {
    auto it = graph.nodes.find(cur);
    if (it == graph.nodes.end()) {
      break;
    }
    const SessionNode& node = it->second;
    out.push_back(node);
    cur = node.parent_id;
  }
  std::reverse(out.begin(), out.end());
  return out;
}

PreparedBranchEntries prepare_branch_entries(const std::vector<SessionNode>& entries, int token_budget) {
  PreparedBranchEntries result;
  for (const auto& entry : entries) {
    merge_row_file_ops(result.file_ops, entry);
  }

  std::vector<ChatMessage> picked;
  int total_tokens = 0;

  for (int i = static_cast<int>(entries.size()) - 1; i >= 0; --i) {
    const SessionNode& entry = entries[static_cast<size_t>(i)];
    ChatMessage msg = entry_to_chat_message(entry);
    if (msg.role.empty()) {
      continue;
    }
    merge_file_ops(result.file_ops, extract_file_ops_from_messages({msg}));

    const int tokens = estimate_chat_tokens(msg);

    if (token_budget > 0 && total_tokens + tokens > token_budget) {
      if ((entry.kind == SessionRowKind::Compaction || entry.kind == SessionRowKind::BranchSummary) &&
          total_tokens < static_cast<int>(static_cast<double>(token_budget) * 0.9)) {
        picked.insert(picked.begin(), std::move(msg));
        total_tokens += tokens;
      }
      break;
    }

    picked.insert(picked.begin(), std::move(msg));
    total_tokens += tokens;
  }

  result.messages = std::move(picked);
  result.total_tokens = total_tokens;
  return result;
}

BranchSummaryResult generate_branch_summary(
    const std::vector<SessionNode>& entries,
    Provider& provider,
    const std::string& model,
    int context_size,
    int reserve_tokens,
    std::string& error
) {
  const int token_budget = std::max(0, context_size - reserve_tokens);
  const PreparedBranchEntries prep = prepare_branch_entries(entries, token_budget);

  if (prep.messages.empty()) {
    BranchSummaryData fb = fallback_snippet_from_entries(entries);
    return BranchSummaryResult{
        .summary = fb.summary,
        .read_files = fb.read_files,
        .modified_files = fb.modified_files,
    };
  }

  const std::string conversation_text = serialize_conversation(prep.messages);
  const std::string prompt_text =
      "<conversation>\n" + conversation_text + "</conversation>\n\n" + std::string(kBranchSummaryPrompt);

  ChatRequest request{
      .messages =
          {
              ChatMessage{
                  .role = "system",
                  .content = kSummarizationSystemPrompt,
                  .tool_call_id = std::nullopt,
                  .tool_calls = {},
                  .entry_id = std::nullopt,
                  .usage_tokens = 0,
              },
              ChatMessage{
                  .role = "user",
                  .content = prompt_text,
                  .tool_call_id = std::nullopt,
                  .tool_calls = {},
                  .entry_id = std::nullopt,
                  .usage_tokens = 0,
              },
          },
      .tools = {},
      .model = model,
      .max_tokens = 2048,
      .temperature = 0.2f,
      .stream = false,
  };

  ChatResponse response{};
  const auto chunk_cb = [](const std::string& /*chunk*/) {};
  if (!provider.chat(request, response, chunk_cb, error, nullptr)) {
    BranchSummaryData fb = fallback_snippet_from_entries(entries);
    return BranchSummaryResult{
        .summary = fb.summary,
        .read_files = fb.read_files,
        .modified_files = fb.modified_files,
    };
  }

  std::string summary = kBranchPreamble + response.content;
  summary += build_file_ops_footer(prep.file_ops);

  return BranchSummaryResult{
      .summary = summary,
      .read_files = sorted_file_list(prep.file_ops.read_files),
      .modified_files = sorted_file_list(prep.file_ops.modified_files),
  };
}

}  // namespace coding_agent

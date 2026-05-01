#include "compaction.hpp"
#include "providers/provider.hpp"

#include <atomic>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace {

bool fail(const char* msg) {
  std::cerr << "FAIL: " << msg << "\n";
  return false;
}

std::string pad_chars(int approx_tokens) {
  const int n = std::max(1, approx_tokens) * 4;
  return std::string(static_cast<size_t>(n), 'p');
}

class FakeProvider final : public coding_agent::Provider {
 public:
  bool chat(
      const coding_agent::ChatRequest& /*request*/,
      coding_agent::ChatResponse& response,
      const coding_agent::ChunkCallback& /*on_chunk*/,
      std::string& /*error*/,
      std::atomic<bool>* /*cancel_flag*/ = nullptr
  ) override {
    response.content = "CANNED_SUMMARY";
    response.tool_calls.clear();
    response.completion_tokens = 10;
    return true;
  }

  void cancel() override {}
};

}  // namespace

int main() {
  std::vector<coding_agent::ChatMessage> messages;
  messages.push_back(coding_agent::ChatMessage{
      .role = "system",
      .content = pad_chars(10),
      .tool_call_id = std::nullopt,
      .tool_calls = {},
      .entry_id = std::nullopt,
      .usage_tokens = 0,
  });
  messages.push_back(coding_agent::ChatMessage{
      .role = "user",
      .content = pad_chars(50),
      .tool_call_id = std::nullopt,
      .tool_calls = {},
      .entry_id = std::nullopt,
      .usage_tokens = 0,
  });
  messages.push_back(coding_agent::ChatMessage{
      .role = "assistant",
      .content = pad_chars(3),
      .tool_call_id = std::nullopt,
      .tool_calls =
          {coding_agent::ToolCall{
              .id = "c1",
              .name = "read",
              .arguments_json = R"({"path":"/window/read.cpp"})",
          }},
      .entry_id = std::nullopt,
      .usage_tokens = 0,
  });
  messages.push_back(coding_agent::ChatMessage{
      .role = "user",
      .content = pad_chars(30),
      .tool_call_id = std::nullopt,
      .tool_calls = {},
      .entry_id = std::nullopt,
      .usage_tokens = 0,
  });
  messages.push_back(coding_agent::ChatMessage{
      .role = "assistant",
      .content = pad_chars(30),
      .tool_call_id = std::nullopt,
      .tool_calls = {},
      .entry_id = std::nullopt,
      .usage_tokens = 0,
  });
  messages.push_back(coding_agent::ChatMessage{
      .role = "user",
      .content = pad_chars(2),
      .tool_call_id = std::nullopt,
      .tool_calls = {},
      .entry_id = std::nullopt,
      .usage_tokens = 0,
  });

  coding_agent::FileOps prior;
  prior.read_files.insert("/prior/carried.txt");
  prior.modified_files.insert("/prior/old_write.txt");

  FakeProvider provider;
  coding_agent::CompactionStats stats{};
  std::string error;
  constexpr int k_keep_recent = 1;
  constexpr int k_reserve = 512;

  if (!coding_agent::compact_history(
          messages,
          provider,
          "fake-model",
          k_keep_recent,
          k_reserve,
          &prior,
          &stats,
          error
      )) {
    std::cerr << "compact_history: " << error << "\n";
    return 1;
  }

  if (!stats.did_compact) {
    return fail("expected compaction to run (tune fixture if this fails)");
  }

  if (stats.file_ops.read_files.count("/prior/carried.txt") != 1U) {
    return fail("prior read_files should carry forward");
  }
  if (stats.file_ops.modified_files.count("/prior/old_write.txt") != 1U) {
    return fail("prior modified_files should carry forward");
  }
  if (stats.file_ops.read_files.count("/window/read.cpp") != 1U) {
    return fail("summarized window should contribute read path");
  }

  const std::string& summary = stats.summary;
  if (summary.find("CANNED_SUMMARY") == std::string::npos) {
    return fail("summary should include provider output");
  }
  if (summary.find("## Files Read") == std::string::npos || summary.find("/window/read.cpp") == std::string::npos ||
      summary.find("/prior/carried.txt") == std::string::npos) {
    return fail("summary footer should list merged read paths");
  }

  std::cout << "coding-agent-compaction-carry-forward-test: ok\n";
  return 0;
}

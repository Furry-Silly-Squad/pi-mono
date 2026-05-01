#include "file_ops.hpp"

#include <cstdlib>
#include <iostream>
#include <vector>

namespace {

bool fail(const char* msg) {
  std::cerr << "FAIL: " << msg << "\n";
  return false;
}

}  // namespace

int main() {
  std::vector<coding_agent::ChatMessage> msgs;
  msgs.push_back(coding_agent::ChatMessage{
      .role = "assistant",
      .content = "",
      .tool_call_id = std::nullopt,
      .tool_calls =
          {
              coding_agent::ToolCall{.id = "1", .name = "read", .arguments_json = R"({"path":"/alpha/read.cpp"})"},
              coding_agent::ToolCall{.id = "2", .name = "write", .arguments_json = R"({"path":"/beta/out.txt"})"},
              coding_agent::ToolCall{.id = "3", .name = "edit", .arguments_json = R"({"path":"/alpha/read.cpp"})"},
              coding_agent::ToolCall{.id = "4", .name = "bash", .arguments_json = R"({"command":"true"})"},
          },
      .entry_id = std::nullopt,
      .usage_tokens = 0,
  });

  coding_agent::FileOps ops = coding_agent::extract_file_ops_from_messages(msgs);
  if (ops.read_files.count("/alpha/read.cpp") != 1U) {
    return fail("expected one read path");
  }
  if (ops.modified_files.count("/beta/out.txt") != 1U || ops.modified_files.count("/alpha/read.cpp") != 1U) {
    return fail("expected write and edit paths in modified_files");
  }

  coding_agent::FileOps prior{};
  prior.read_files.insert("/prior/seed.txt");
  prior.modified_files.insert("/prior/old.txt");
  coding_agent::merge_file_ops(ops, prior);
  if (ops.read_files.count("/prior/seed.txt") != 1U || ops.modified_files.count("/prior/old.txt") != 1U) {
    return fail("merge_file_ops should union sets");
  }

  const std::string footer = coding_agent::build_file_ops_footer(ops);
  if (footer.find("\n\n## Files Read\n") == std::string::npos) {
    return fail("footer should contain Files Read section");
  }
  if (footer.find("\n## Files Modified\n") == std::string::npos) {
    return fail("footer should contain Files Modified section");
  }
  // Sorted paths: /alpha/read.cpp appears in read list; modified list sorted separately
  if (footer.find("- /alpha/read.cpp") == std::string::npos || footer.find("- /beta/out.txt") == std::string::npos) {
    return fail("footer should list known paths");
  }

  std::cout << "coding-agent-fileops-test: ok\n";
  return 0;
}

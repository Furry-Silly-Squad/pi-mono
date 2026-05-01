#include "branch_summary.hpp"
#include "session.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <unistd.h>

namespace {

bool fail(const char* msg) {
  std::cerr << "FAIL: " << msg << "\n";
  return false;
}

}  // namespace

int main() {
  namespace fs = std::filesystem;

  const fs::path dir =
      fs::temp_directory_path() / ("coding-agent-branch-test-" + std::to_string(::getpid()));
  fs::create_directories(dir);

  const fs::path session_file = dir / "tree.jsonl";
  {
    std::ofstream out(session_file);
    out << R"({"type":"session","id":"root","parent_id":""})" << "\n";
    out << R"({"type":"message","id":"m1","parent_id":"root","role":"user","content":"hi"})" << "\n";
    out << R"({"type":"message","id":"m2","parent_id":"m1","role":"assistant","content":"hello"})" << "\n";
    out << R"({"type":"message","id":"m3","parent_id":"m2","role":"user","content":"fork-a"})" << "\n";
    out << R"({"type":"message","id":"m4","parent_id":"m2","role":"user","content":"fork-b"})" << "\n";
  }

  coding_agent::SessionGraph g;
  std::string err;
  if (!coding_agent::load_session_graph(session_file.string(), g, err)) {
    std::cerr << "load_session_graph: " << err << "\n";
    return 1;
  }
  if (g.leaf_id != "m4") {
    return fail("leaf_id should be last row id");
  }

  const auto branch_m4 = g.get_branch("m4");
  if (branch_m4.size() != 4U || branch_m4[0] != "root" || branch_m4.back() != "m4") {
    return fail("get_branch m4 path");
  }

  const auto anc = g.find_common_ancestor("m3", "m4");
  if (anc != "m2") {
    return fail("common ancestor of fork branches should be m2");
  }

  const auto from_leaf = coding_agent::collect_entries_for_branch_summary(g, "m3", "");
  if (from_leaf.size() != 3U) {
    return fail("collect to root should include m1,m2,m3");
  }
  if (from_leaf[0].id != "m1" || from_leaf[2].id != "m3") {
    return fail("collect chronological order");
  }

  const auto between = coding_agent::collect_entries_for_branch_summary(g, "m3", "m4");
  if (between.size() != 1U || between[0].id != "m3") {
    return fail("collect between m3 and m4 should be just m3");
  }

  const auto prepared =
      coding_agent::prepare_branch_entries(from_leaf, 100000);
  if (prepared.messages.empty()) {
    return fail("prepare should include messages");
  }

  std::cout << "coding-agent-branch-traversal-test: ok\n";
  return 0;
}

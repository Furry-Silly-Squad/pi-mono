#include "branch_summary.hpp"
#include "session_entry.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <unordered_set>
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

  auto mgr = coding_agent::SessionManager::open(session_file.string(), "", "");
  if (!mgr) {
    std::cerr << "Failed to open session\n";
    return 1;
  }

  if (mgr->getLeafId().value_or("") != "m4") {
    return fail("leaf_id should be last row id");
  }

  const auto branch_m4 = mgr->getBranch("m4");
  // Session header id "root" is not stored as a SessionEntry in byId; path is message ids only.
  if (branch_m4.size() != 3U) {
    return fail("get_branch m4 path size");
  }
  {
    const std::string first_id = std::visit(
        [](const auto& e) -> std::string { return e.id; }, branch_m4[0]);
    if (first_id != "m1") {
      return fail("get_branch m4 first entry should be m1");
    }
  }
  {
    const std::string last_id = std::visit(
        [](const auto& e) -> std::string { return e.id; }, branch_m4.back());
    if (last_id != "m4") {
      return fail("get_branch m4 last entry should be m4");
    }
  }

  // Common ancestor: walk up from m3 and m4, find first shared id.
  const auto path_m3 = mgr->getBranch("m3");
  const auto path_m4 = mgr->getBranch("m4");
  std::unordered_set<std::string> m3_ids;
  for (const auto& e : path_m3) {
    m3_ids.insert(std::visit([](const auto& ent) -> const std::string& { return ent.id; }, e));
  }
  std::string anc = "";
  for (auto it = path_m4.rbegin(); it != path_m4.rend(); ++it) {
    const std::string id =
        std::visit([](const auto& e) -> std::string { return e.id; }, *it);
    if (m3_ids.count(id)) {
      anc = id;
      break;
    }
  }
  if (anc != "m2") {
    return fail("common ancestor of fork branches should be m2");
  }

  const auto from_leaf = coding_agent::collect_entries_for_branch_summary(*mgr, "m3", "");
  if (from_leaf.size() != 3U) {
    return fail("collect to root should include m1,m2,m3");
  }
  {
    const std::string first_id =
        std::visit([](const auto& e) -> std::string { return e.id; }, from_leaf[0]);
    if (first_id != "m1") {
      return fail("collect chronological order first");
    }
  }
  {
    const std::string last_id =
        std::visit([](const auto& e) -> std::string { return e.id; }, from_leaf[2]);
    if (last_id != "m3") {
      return fail("collect chronological order last");
    }
  }

  const auto between = coding_agent::collect_entries_for_branch_summary(*mgr, "m3", "m4");
  if (between.size() != 1U) {
    return fail("collect between m3 and m4 should be just m3");
  }
  {
    const std::string id =
        std::visit([](const auto& e) -> std::string { return e.id; }, between[0]);
    if (id != "m3") {
      return fail("collect between m3 and m4 should be m3");
    }
  }

  const auto prepared =
      coding_agent::prepare_branch_entries(from_leaf, 100000);
  if (prepared.messages.empty()) {
    return fail("prepare should include messages");
  }

  std::cout << "coding-agent-branch-traversal-test: ok\n";
  return 0;
}

#include "context_loader.hpp"

#include <filesystem>
#include <fstream>
#include <sstream>

namespace coding_agent {
namespace {

std::string read_text_file(const std::filesystem::path& path) {
  std::ifstream input(path);
  std::stringstream buffer;
  buffer << input.rdbuf();
  return buffer.str();
}

}  // namespace

std::vector<ContextFile> load_context_files(const std::string& start_cwd) {
  std::vector<ContextFile> files;
  std::filesystem::path current = std::filesystem::absolute(start_cwd);

  while (true) {
    const std::filesystem::path agents = current / "AGENTS.md";
    if (std::filesystem::exists(agents)) {
      files.push_back(ContextFile{
          .path = agents.string(),
          .content = read_text_file(agents),
      });
    }

    const std::filesystem::path claude = current / "CLAUDE.md";
    if (std::filesystem::exists(claude)) {
      files.push_back(ContextFile{
          .path = claude.string(),
          .content = read_text_file(claude),
      });
    }

    if (!current.has_parent_path() || current == current.parent_path()) {
      break;
    }
    current = current.parent_path();
  }

  return files;
}

}  // namespace coding_agent

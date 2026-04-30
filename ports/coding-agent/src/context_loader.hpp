#pragma once

#include <string>
#include <vector>

namespace coding_agent {

struct ContextFile {
  std::string path;
  std::string content;
};

std::vector<ContextFile> load_context_files(const std::string& start_cwd);

}  // namespace coding_agent

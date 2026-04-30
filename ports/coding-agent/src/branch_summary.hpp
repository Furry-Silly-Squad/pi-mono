#pragma once

#include <filesystem>
#include <string>

namespace coding_agent {

std::string summarize_branch_session_file(const std::filesystem::path& session_path);

}  // namespace coding_agent

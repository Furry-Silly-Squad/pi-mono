#pragma once

#include <string>
#include <unordered_set>
#include <vector>

#include "providers/provider.hpp"

namespace coding_agent {

struct FileOps {
  std::unordered_set<std::string> read_files;
  std::unordered_set<std::string> modified_files;
};

FileOps extract_file_ops_from_messages(const std::vector<ChatMessage>& messages);
void merge_file_ops(FileOps& dst, const FileOps& src);

std::vector<std::string> sorted_file_list(const std::unordered_set<std::string>& files);
std::string build_file_ops_footer(const FileOps& file_ops);

}  // namespace coding_agent

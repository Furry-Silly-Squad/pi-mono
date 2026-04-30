#pragma once

#include <optional>
#include <string>

namespace coding_agent {

struct Config {
  std::string provider;
  std::string base_url;
  std::string model;
  std::string api_key;
  int n_predict;
  float temperature;
  bool stream;
};

std::optional<Config> parse_config(int argc, char** argv, std::string& error);
void print_usage();

}  // namespace coding_agent

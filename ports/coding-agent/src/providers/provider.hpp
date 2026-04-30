#pragma once

#include <functional>
#include <string>

namespace coding_agent {

struct GenerationRequest {
  std::string prompt;
  std::string model;
  int n_predict;
  float temperature;
  bool stream;
};

using ChunkCallback = std::function<void(const std::string&)>;

class Provider {
 public:
  virtual ~Provider() = default;
  virtual bool generate(
      const GenerationRequest& request,
      const ChunkCallback& on_chunk,
      std::string& error
  ) = 0;
};

}  // namespace coding_agent

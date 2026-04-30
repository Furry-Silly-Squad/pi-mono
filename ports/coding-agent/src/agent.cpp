#include "agent.hpp"

#include <iostream>
#include <memory>
#include <string>

#include "config.hpp"
#include "providers/llama_cpp_provider.hpp"
#include "providers/provider.hpp"

namespace coding_agent {
namespace {

std::string extract_prompt(int argc, char** argv) {
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--prompt" && i + 1 < argc) {
      return argv[i + 1];
    }
  }
  return "";
}

}  // namespace

int run_agent(int argc, char** argv) {
  std::string parse_error;
  auto config = parse_config(argc, argv, parse_error);
  if (!config.has_value()) {
    if (!parse_error.empty()) {
      std::cerr << "Error: " << parse_error << "\n\n";
      print_usage();
      return 1;
    }
    return 0;
  }

  if (config->provider != "llama-cpp") {
    std::cerr << "Error: unsupported provider '" << config->provider
              << "'. This port currently supports only llama-cpp.\n";
    return 1;
  }

  const std::string prompt = extract_prompt(argc, argv);
  if (prompt.empty()) {
    std::cerr << "Error: prompt is empty.\n";
    return 1;
  }

  std::unique_ptr<Provider> provider =
      std::make_unique<LlamaCppProvider>(config->base_url, config->api_key);
  GenerationRequest request{
      .prompt = prompt,
      .model = config->model,
      .n_predict = config->n_predict,
      .temperature = config->temperature,
      .stream = config->stream,
  };

  std::string generation_error;
  const bool ok = provider->generate(
      request,
      [](const std::string& chunk) { std::cout << chunk << std::flush; },
      generation_error
  );

  if (!ok) {
    std::cerr << "\nError: " << generation_error << "\n";
    return 1;
  }

  std::cout << "\n";
  return 0;
}

}  // namespace coding_agent

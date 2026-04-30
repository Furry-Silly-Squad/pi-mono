#include "providers/llama_cpp_provider.hpp"

#include <curl/curl.h>

#include <regex>
#include <sstream>
#include <string>
#include <utility>

namespace coding_agent {
namespace {

std::string trim_trailing_slash(const std::string& input) {
  if (!input.empty() && input.back() == '/') {
    return input.substr(0, input.size() - 1);
  }
  return input;
}

std::string escape_json(const std::string& input) {
  std::string escaped;
  escaped.reserve(input.size());
  for (const char c : input) {
    switch (c) {
      case '\\':
        escaped += "\\\\";
        break;
      case '"':
        escaped += "\\\"";
        break;
      case '\n':
        escaped += "\\n";
        break;
      case '\r':
        escaped += "\\r";
        break;
      case '\t':
        escaped += "\\t";
        break;
      default:
        escaped += c;
        break;
    }
  }
  return escaped;
}

std::string extract_first_match(const std::string& body, const std::regex& pattern) {
  std::smatch match;
  if (std::regex_search(body, match, pattern) && match.size() > 1) {
    return match[1].str();
  }
  return "";
}

std::string trim(const std::string& input) {
  size_t start = 0;
  while (start < input.size() && (input[start] == ' ' || input[start] == '\n' || input[start] == '\r')) {
    ++start;
  }
  size_t end = input.size();
  while (end > start && (input[end - 1] == ' ' || input[end - 1] == '\n' || input[end - 1] == '\r')) {
    --end;
  }
  return input.substr(start, end - start);
}

std::string unescape_basic_json_string(const std::string& input) {
  std::string result;
  result.reserve(input.size());
  for (size_t i = 0; i < input.size(); ++i) {
    if (input[i] == '\\' && i + 1 < input.size()) {
      const char next = input[i + 1];
      switch (next) {
        case 'n':
          result.push_back('\n');
          ++i;
          continue;
        case 'r':
          result.push_back('\r');
          ++i;
          continue;
        case 't':
          result.push_back('\t');
          ++i;
          continue;
        case '\\':
          result.push_back('\\');
          ++i;
          continue;
        case '"':
          result.push_back('"');
          ++i;
          continue;
        default:
          break;
      }
    }
    result.push_back(input[i]);
  }
  return result;
}

std::string parse_text_from_response(const std::string& body) {
  // Prefer OpenAI-compatible chat completion shape.
  // Example: choices[0].message.content
  // Streaming chunks: choices[0].delta.content
  static const std::regex message_content_pattern(
      R"("message"\s*:\s*\{[^{}]*"content"\s*:\s*"((?:\\.|[^"\\])*)")"
  );
  static const std::regex delta_content_pattern(
      R"("delta"\s*:\s*\{[^{}]*"content"\s*:\s*"((?:\\.|[^"\\])*)")"
  );
  static const std::regex content_pattern(R"("content"\s*:\s*"((?:\\.|[^"\\])*)")");
  static const std::regex text_pattern(R"("text"\s*:\s*"((?:\\.|[^"\\])*)")");

  std::string text = extract_first_match(body, message_content_pattern);
  if (text.empty()) {
    text = extract_first_match(body, delta_content_pattern);
  }
  if (text.empty()) {
    text = extract_first_match(body, content_pattern);
  }
  if (text.empty()) {
    text = extract_first_match(body, text_pattern);
  }
  return text.empty() ? "" : unescape_basic_json_string(text);
}

bool is_sse_done(const std::string& json_line) {
  const std::string cleaned = trim(json_line);
  return cleaned == "[DONE]" || cleaned == "\"[DONE]\"";
}

struct StreamState {
  std::string buffer;
  ChunkCallback on_chunk;
};

size_t write_non_stream_callback(void* contents, size_t size, size_t nmemb, void* userp) {
  const size_t total_size = size * nmemb;
  auto* buffer = static_cast<std::string*>(userp);
  buffer->append(static_cast<char*>(contents), total_size);
  return total_size;
}

size_t write_stream_callback(void* contents, size_t size, size_t nmemb, void* userp) {
  const size_t total_size = size * nmemb;
  auto* state = static_cast<StreamState*>(userp);
  state->buffer.append(static_cast<char*>(contents), total_size);

  size_t line_start = 0;
  while (true) {
    const size_t line_end = state->buffer.find('\n', line_start);
    if (line_end == std::string::npos) {
      // Keep unprocessed tail for next callback.
      state->buffer.erase(0, line_start);
      break;
    }

    std::string line = state->buffer.substr(line_start, line_end - line_start);
    line_start = line_end + 1;

    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    if (line.rfind("data:", 0) != 0) {
      continue;
    }

    const std::string data = trim(line.substr(5));
    if (data.empty() || is_sse_done(data)) {
      continue;
    }

    const std::string text = parse_text_from_response(data);
    if (!text.empty()) {
      state->on_chunk(text);
    }
  }

  return total_size;
}

}  // namespace

LlamaCppProvider::LlamaCppProvider(std::string base_url, std::string api_key)
    : base_url_(trim_trailing_slash(base_url)), api_key_(std::move(api_key)) {}

bool LlamaCppProvider::generate(
    const GenerationRequest& request,
    const ChunkCallback& on_chunk,
    std::string& error
) {
  CURL* curl = curl_easy_init();
  if (curl == nullptr) {
    error = "Failed to initialize curl";
    return false;
  }

  std::string response_body;
  struct curl_slist* headers = nullptr;
  headers = curl_slist_append(headers, "Content-Type: application/json");
  std::string auth_header;
  if (!api_key_.empty()) {
    auth_header = "Authorization: Bearer " + api_key_;
    headers = curl_slist_append(headers, auth_header.c_str());
  }

  const std::string endpoint = base_url_ + "/v1/chat/completions";
  std::ostringstream payload;
  payload << "{"
          << "\"messages\":[{\"role\":\"user\",\"content\":\"" << escape_json(request.prompt) << "\"}],"
          << "\"stream\":" << (request.stream ? "true" : "false") << ","
          << "\"max_tokens\":" << request.n_predict << ","
          << "\"temperature\":" << request.temperature;
  if (!request.model.empty()) {
    payload << ",\"model\":\"" << escape_json(request.model) << "\"";
  }
  payload << "}";

  curl_easy_setopt(curl, CURLOPT_URL, endpoint.c_str());
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
  curl_easy_setopt(curl, CURLOPT_POST, 1L);
  curl_easy_setopt(curl, CURLOPT_POSTFIELDS, payload.str().c_str());
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, 300L);
  curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, payload.str().size());

  StreamState stream_state{.buffer = "", .on_chunk = on_chunk};
  if (request.stream) {
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_stream_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &stream_state);
  } else {
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_non_stream_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response_body);
  }

  const CURLcode result = curl_easy_perform(curl);
  long http_status = 0;
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_status);

  curl_slist_free_all(headers);
  curl_easy_cleanup(curl);

  if (result != CURLE_OK) {
    error = std::string("curl request failed: ") + curl_easy_strerror(result);
    return false;
  }

  if (http_status < 200 || http_status >= 300) {
    error = "llama.cpp returned HTTP " + std::to_string(http_status) + ": " + response_body;
    return false;
  }

  if (request.stream) {
    return true;
  }

  const std::string text = parse_text_from_response(response_body);
  if (text.empty()) {
    error = "Unable to parse text from llama.cpp response: " + response_body;
    return false;
  }

  on_chunk(text);
  return true;
}

}  // namespace coding_agent

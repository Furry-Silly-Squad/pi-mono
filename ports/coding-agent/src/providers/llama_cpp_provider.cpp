#include "providers/llama_cpp_provider.hpp"

#include <curl/curl.h>

#include <nlohmann/json.hpp>

#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace coding_agent {
namespace {

using nlohmann::json;

std::string trim_trailing_slash(const std::string& input) {
  if (!input.empty() && input.back() == '/') {
    return input.substr(0, input.size() - 1);
  }
  return input;
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

bool is_sse_done(const std::string& json_line) {
  const std::string cleaned = trim(json_line);
  return cleaned == "[DONE]" || cleaned == "\"[DONE]\"";
}

struct StreamState {
  std::string buffer;
  ChatResponse* response;
  ChunkCallback on_chunk;
  std::string error;
};

bool is_valid_json_value(const std::string& raw) {
  const json parsed = json::parse(raw, nullptr, false);
  return !parsed.is_discarded();
}

size_t write_non_stream_callback(void* contents, size_t size, size_t nmemb, void* userp) {
  const size_t total_size = size * nmemb;
  auto* buffer = static_cast<std::string*>(userp);
  buffer->append(static_cast<char*>(contents), total_size);
  return total_size;
}

bool parse_tool_calls(const json& tool_calls_json, std::vector<ToolCall>& out, std::string& error) {
  out.clear();
  if (!tool_calls_json.is_array()) {
    return true;
  }
  for (const auto& item : tool_calls_json) {
    ToolCall call;
    call.id = item.value("id", "");
    if (item.contains("function")) {
      const auto& fn = item.at("function");
      call.name = fn.value("name", "");
      if (fn.contains("arguments")) {
        if (fn.at("arguments").is_string()) {
          call.arguments_json = fn.at("arguments").get<std::string>();
        } else {
          call.arguments_json = fn.at("arguments").dump();
        }
      }
    }
    if (!call.name.empty()) {
      if (call.arguments_json.empty() || !is_valid_json_value(call.arguments_json)) {
        error = "Invalid tool call arguments JSON for tool '" + call.name + "'";
        return false;
      }
      out.push_back(std::move(call));
    }
  }
  return true;
}

void merge_stream_tool_calls(
    std::vector<ToolCall>& aggregate,
    const json& stream_tool_calls
) {
  if (!stream_tool_calls.is_array()) {
    return;
  }

  for (const auto& piece : stream_tool_calls) {
    const size_t index = piece.value("index", 0U);
    while (aggregate.size() <= index) {
      aggregate.push_back(ToolCall{});
    }
    ToolCall& call = aggregate[index];
    if (piece.contains("id") && piece.at("id").is_string()) {
      call.id = piece.at("id").get<std::string>();
    }
    if (piece.contains("function")) {
      const auto& fn = piece.at("function");
      if (fn.contains("name") && fn.at("name").is_string()) {
        call.name += fn.at("name").get<std::string>();
      }
      if (fn.contains("arguments") && fn.at("arguments").is_string()) {
        call.arguments_json += fn.at("arguments").get<std::string>();
      }
    }
  }
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

    try {
      const json parsed = json::parse(data);
      if (!parsed.contains("choices") || !parsed.at("choices").is_array() ||
          parsed.at("choices").empty()) {
        continue;
      }
      const auto& choice = parsed.at("choices")[0];
      if (choice.contains("delta")) {
        const auto& delta = choice.at("delta");
        if (delta.contains("content") && delta.at("content").is_string()) {
          const std::string chunk = delta.at("content").get<std::string>();
          state->response->content += chunk;
          state->on_chunk(chunk);
        }
        if (delta.contains("tool_calls")) {
          merge_stream_tool_calls(state->response->tool_calls, delta.at("tool_calls"));
        }
      }
    } catch (const std::exception& ex) {
      state->error = ex.what();
    }
  }

  return total_size;
}

}  // namespace

json to_json_message(const ChatMessage& message) {
  json m{
      {"role", message.role},
      {"content", message.content},
  };
  if (message.tool_call_id.has_value()) {
    m["tool_call_id"] = message.tool_call_id.value();
  }
  if (!message.tool_calls.empty()) {
    m["tool_calls"] = json::array();
    for (const auto& call : message.tool_calls) {
      // Guard against malformed historical tool call args causing provider 500s.
      if (call.arguments_json.empty() || !is_valid_json_value(call.arguments_json)) {
        continue;
      }
      m["tool_calls"].push_back(
          json{
              {"id", call.id},
              {"type", "function"},
              {"function",
               {
                   {"name", call.name},
                   {"arguments", call.arguments_json},
               }},
          }
      );
    }
  }
  return m;
}

json to_json_tool(const ToolDefinition& tool) {
  json params = json::object();
  try {
    params = json::parse(tool.parameters_schema_json);
  } catch (...) {
    params = json::object();
  }
  return json{
      {"type", "function"},
      {"function",
       {
           {"name", tool.name},
           {"description", tool.description},
           {"parameters", params},
       }},
  };
}

LlamaCppProvider::LlamaCppProvider(std::string base_url, std::string api_key)
    : base_url_(trim_trailing_slash(base_url)), api_key_(std::move(api_key)) {}

bool LlamaCppProvider::chat(
    const ChatRequest& request,
    ChatResponse& response,
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
  json payload{
      {"messages", json::array()},
      {"stream", request.stream},
      {"max_tokens", request.max_tokens},
      {"temperature", request.temperature},
  };
  if (!request.model.empty()) {
    payload["model"] = request.model;
  }
  if (!request.tools.empty()) {
    payload["tools"] = json::array();
    for (const auto& tool : request.tools) {
      payload["tools"].push_back(to_json_tool(tool));
    }
  }
  for (const auto& message : request.messages) {
    payload["messages"].push_back(to_json_message(message));
  }
  const std::string payload_text = payload.dump();

  curl_easy_setopt(curl, CURLOPT_URL, endpoint.c_str());
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
  curl_easy_setopt(curl, CURLOPT_POST, 1L);
  curl_easy_setopt(curl, CURLOPT_POSTFIELDS, payload_text.c_str());
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, 300L);
  curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, payload_text.size());

  StreamState stream_state{.buffer = "", .response = &response, .on_chunk = on_chunk, .error = ""};
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
    if (!stream_state.error.empty()) {
      error = "stream parsing error: " + stream_state.error;
      return false;
    }
    for (const auto& call : response.tool_calls) {
      if (call.name.empty()) {
        continue;
      }
      if (call.arguments_json.empty() || !is_valid_json_value(call.arguments_json)) {
        error = "Invalid streamed tool call arguments JSON for tool '" + call.name + "'";
        return false;
      }
    }
    return true;
  }

  try {
    const json parsed = json::parse(response_body);
    if (!parsed.contains("choices") || !parsed.at("choices").is_array() ||
        parsed.at("choices").empty()) {
      error = "invalid chat completion response: " + response_body;
      return false;
    }
    const auto& message = parsed.at("choices")[0].at("message");
    response.content = message.value("content", "");
    if (message.contains("tool_calls")) {
      std::vector<ToolCall> parsed_tool_calls;
      if (!parse_tool_calls(message.at("tool_calls"), parsed_tool_calls, error)) {
        return false;
      }
      response.tool_calls = std::move(parsed_tool_calls);
    }
  } catch (const std::exception& ex) {
    error = std::string("Unable to parse llama.cpp response: ") + ex.what();
    return false;
  }

  if (!response.content.empty()) {
    on_chunk(response.content);
  }
  return true;
}

}  // namespace coding_agent

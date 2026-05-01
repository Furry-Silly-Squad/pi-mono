#include "providers/llama_cpp_provider.hpp"

#include <curl/curl.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <iostream>
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
  std::vector<size_t> tool_call_fragment_counts;
  bool usage_extracted = false;
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
    StreamState* state,
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
    while (state != nullptr && state->tool_call_fragment_counts.size() <= index) {
      state->tool_call_fragment_counts.push_back(0U);
    }
    ToolCall& call = aggregate[index];
    if (piece.contains("id") && piece.at("id").is_string()) {
      call.id = piece.at("id").get<std::string>();
    }
    if (piece.contains("function")) {
      const auto& fn = piece.at("function");
      if (fn.contains("name") && fn.at("name").is_string()) {
        const std::string name_fragment = fn.at("name").get<std::string>();
        call.name += name_fragment;
      }
      if (fn.contains("arguments") && fn.at("arguments").is_string()) {
        const std::string arguments_fragment = fn.at("arguments").get<std::string>();
        call.arguments_json += arguments_fragment;
        if (state != nullptr) {
          ++state->tool_call_fragment_counts[index];
        }
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
          merge_stream_tool_calls(state, state->response->tool_calls, delta.at("tool_calls"));
        }
      }
      // Extract usage from the last chunk (sent in the final SSE event)
      if (!state->usage_extracted && parsed.contains("usage")) {
        const auto& usage = parsed.at("usage");
        state->response->prompt_tokens = usage.value("prompt_tokens", 0);
        state->response->completion_tokens = usage.value("completion_tokens", 0);
        state->usage_extracted = true;
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
        const std::string failing_tool_name = call.name;
        const std::string failing_tool_id = call.id;
        const size_t total_size = call.arguments_json.size();
        const size_t tail_size = std::min<size_t>(300, total_size);
        const size_t tail_start = total_size > tail_size ? total_size - tail_size : 0;
        std::string tail = call.arguments_json.substr(tail_start);
        size_t index = 0;
        for (; index < response.tool_calls.size(); ++index) {
          if (response.tool_calls[index].id == call.id) {
            break;
          }
        }
        size_t fragments = 0;
        if (index < stream_state.tool_call_fragment_counts.size()) {
          fragments = stream_state.tool_call_fragment_counts[index];
        }
        std::cerr << "[debug][tool-stream-invalid] id=" << call.id << " name=" << call.name
                  << " index=" << index << " fragment_count=" << fragments
                  << " arguments_size=" << total_size << "\n";
        std::cerr << "[debug][tool-stream-invalid] full_arguments_json:\n"
                  << call.arguments_json << "\n";
        std::cerr << "[debug][tool-stream-invalid] tail_300:\n" << tail << "\n";

        // Fallback: retry once in non-stream mode to recover from truncated SSE tool-call args.
        ChatRequest retry_request = request;
        retry_request.stream = false;
        // Streamed tool-call JSON for `edit`/`write` can be tens of KB; raise completion budget.
        constexpr int k_fallback_floor_tokens = 16384;
        constexpr int k_fallback_cap_tokens = 65536;
        const long long scaled =
            static_cast<long long>(retry_request.max_tokens) * 4LL;
        retry_request.max_tokens = static_cast<int>(
            std::min<long long>(k_fallback_cap_tokens, std::max<long long>(scaled, k_fallback_floor_tokens))
        );
        ChatResponse retry_response;
        std::string retry_error;
        if (chat(retry_request, retry_response, on_chunk, retry_error)) {
          response = std::move(retry_response);
          std::cerr << "[debug][tool-stream-fallback] recovered via non-stream retry for tool '"
                    << failing_tool_name << "' id=" << failing_tool_id << "\n";
          return true;
        }

        error = "Invalid streamed tool call arguments JSON for tool '" + failing_tool_name +
                "' (tail: " + tail + "); fallback non-stream failed: " + retry_error;
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
    if (parsed.contains("usage")) {
      const auto& usage = parsed.at("usage");
      response.prompt_tokens = usage.value("prompt_tokens", 0);
      response.completion_tokens = usage.value("completion_tokens", 0);
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

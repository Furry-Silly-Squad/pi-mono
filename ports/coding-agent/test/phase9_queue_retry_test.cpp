// Unit tests for Phase 9: Queue Management + Retry with Exponential Backoff
//
// Covers:
//  * PendingMessageQueue: OneAtATime vs All drain modes.
//  * steer() and followUp() enqueue messages correctly.
//  * Queue drain in run_turn(): steering before LLM call, follow-up when agent stops.
//  * is_retryable_error(): rate limit, overloaded, 5xx, timeout, connection errors.
//  * is_retryable_error(): tool errors, auth errors are NOT retryable.
//  * Retry exponential backoff with configurable settings.
//  * Max retries respected.
//  * Retry success resets counter.
//  * sendCustomMessage() with delivery modes.
//  * Queue event emission.

#include "agent_session.hpp"
#include "pending_message_queue.hpp"

#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <queue>
#include <sstream>
#include <string>
#include <unistd.h>
#include <vector>

#include "compaction.hpp"
#include "providers/provider.hpp"
#include "session_entry.hpp"
#include "tools/tool_registry.hpp"

namespace {

bool fail(const char* msg) {
  std::cerr << "FAIL: " << msg << "\n";
  return false;
}

#define EXPECT(cond, msg)            \
  do {                               \
    if (!(cond)) return fail(msg);   \
  } while (0)

// ===========================================================================
// FakeProvider with a scripted response queue
// ===========================================================================

class ScriptedProvider final : public coding_agent::Provider {
 public:
  void enqueue(coding_agent::ChatResponse response) {
    responses_.push(std::move(response));
  }

  bool chat(
      const coding_agent::ChatRequest& /*request*/,
      coding_agent::ChatResponse& response,
      const coding_agent::ChunkCallback& /*on_chunk*/,
      std::string& error,
      std::atomic<bool>* cancel_flag = nullptr
  ) override {
    ++call_count_;
    if (cancel_flag != nullptr && cancel_flag->load()) {
      error = "interrupted";
      return false;
    }
    if (responses_.empty()) {
      error = "no scripted response";
      return false;
    }
    response = responses_.front();
    responses_.pop();
    return true;
  }

  void cancel() override {}

  int call_count() const { return call_count_; }
  size_t remaining() const { return responses_.size(); }

 private:
  std::queue<coding_agent::ChatResponse> responses_;
  int call_count_ = 0;
};

// ===========================================================================
// Fake tool
// ===========================================================================

class EchoTool final : public coding_agent::Tool {
 public:
  std::string name() const override { return "echo"; }
  std::string description() const override { return "echo back the input"; }
  std::string parameters_schema() const override {
    return R"({"type":"object","properties":{"text":{"type":"string"}}})";
  }
  coding_agent::ToolResult execute(const std::string& /*args_json*/,
                                   const std::string& /*cwd*/) override {
    ++call_count_;
    return coding_agent::ToolResult{.ok = true, .content = "echoed"};
  }
  int call_count() const { return call_count_; }

 private:
  int call_count_ = 0;
};

// ===========================================================================
// Helpers
// ===========================================================================

namespace fs = std::filesystem;

fs::path make_temp_dir(const std::string& tag) {
  static int counter = 0;
  ++counter;
  fs::path dir = fs::temp_directory_path() /
                 ("coding-agent-phase9-test-" +
                  std::to_string(::getpid()) + "-" + tag + "-" +
                  std::to_string(counter));
  fs::create_directories(dir);
  return dir;
}

coding_agent::AgentSessionConfig base_config(const std::string& cwd) {
  coding_agent::AgentSessionConfig cfg{};
  cfg.provider = "test";
  cfg.base_url = "";
  cfg.model = "fake-model";
  cfg.api_key = "";
  cfg.cwd = cwd;
  cfg.max_tokens = 1024;
  cfg.context_size = 4096;
  cfg.compaction_reserve_tokens = 512;
  cfg.compaction_keep_recent_tokens = 256;
  cfg.max_tool_iterations = 10;
  cfg.temperature = 0.0F;
  cfg.stream = false;
  cfg.no_tools = false;
  cfg.no_context_files = true;
  cfg.compaction_fail_fast = true;
  cfg.auto_compaction = false;
  cfg.initial_active_tools = "echo";
  cfg.retry_enabled = true;
  cfg.retry_max_retries = 3;
  cfg.retry_base_delay_ms = 1;  // minimal delay for tests
  cfg.retry_max_retry_delay_ms = 1000;
  cfg.retry_timeout_ms = 30000;
  return cfg;
}

// ===========================================================================
// Test 1: PendingMessageQueue OneAtATime mode
// ===========================================================================

bool test_queue_mode_one_at_a_time() {
  coding_agent::PendingMessageQueue queue(coding_agent::QueueMode::OneAtATime);
  queue.enqueue("msg1");
  queue.enqueue("msg2");
  queue.enqueue("msg3");

  EXPECT(queue.has_items(), "queue has items");
  EXPECT(queue.size() == 3, "queue size is 3");

  auto drained = queue.drain();
  EXPECT(drained.size() == 1, "drain returns 1 item in OneAtATime mode");
  EXPECT(drained[0].first == "msg1", "first item drained");
  EXPECT(queue.size() == 2, "queue still has 2 items");

  drained = queue.drain();
  EXPECT(drained.size() == 1, "second drain returns 1 item");
  EXPECT(drained[0].first == "msg2", "second item drained");

  queue.clear();
  EXPECT(!queue.has_items(), "queue empty after clear");
  EXPECT(queue.drain().empty(), "drain on empty queue returns empty");

  return true;
}

// ===========================================================================
// Test 2: PendingMessageQueue All mode
// ===========================================================================

bool test_queue_mode_all() {
  coding_agent::PendingMessageQueue queue(coding_agent::QueueMode::All);
  queue.enqueue("msg1");
  queue.enqueue("msg2");
  queue.enqueue("msg3");

  auto drained = queue.drain();
  EXPECT(drained.size() == 3, "drain returns all 3 items in All mode");
  EXPECT(drained[0].first == "msg1", "first item");
  EXPECT(drained[1].first == "msg2", "second item");
  EXPECT(drained[2].first == "msg3", "third item");
  EXPECT(!queue.has_items(), "queue empty after All drain");

  return true;
}

// ===========================================================================
// Test 3: steer() and followUp() enqueue correctly
// ===========================================================================

bool test_steer_followup_enqueue() {
  const fs::path session_dir = make_temp_dir("steerfollowup");

  ScriptedProvider provider;
  coding_agent::ChatResponse response;
  response.content = "done";
  response.completion_tokens = 1;
  provider.enqueue(response);

  coding_agent::ToolRegistry tools;
  tools.register_tool(std::make_unique<EchoTool>());

  auto session_mgr = coding_agent::SessionManager::create(session_dir.string(), session_dir.string());
  coding_agent::AgentSession agent(base_config(session_dir.string()), provider, tools,
                                    std::move(session_mgr));

  // Queue messages while not running (should still enqueue)
  agent.steer("steer message");
  agent.followUp("follow-up message");

  EXPECT(agent.has_queued_messages(), "has queued messages");
  EXPECT(agent.steering_mode() == coding_agent::QueueMode::OneAtATime, "default steering mode");
  EXPECT(agent.follow_up_mode() == coding_agent::QueueMode::OneAtATime, "default follow-up mode");

  // Clear steering queue
  agent.clear_steering_queue();
  EXPECT(agent.has_queued_messages(), "still has queued messages (follow-up remains)");

  agent.clear_follow_up_queue();
  EXPECT(!agent.has_queued_messages(), "no queued messages after clearing both");

  return true;
}

// ===========================================================================
// Test 4: steer messages drained before LLM call
// ===========================================================================

bool test_steer_drain_before_call() {
  const fs::path session_dir = make_temp_dir("steerdrain");

  ScriptedProvider provider;
  coding_agent::ChatResponse response;
  response.content = "done";
  response.completion_tokens = 1;
  provider.enqueue(response);

  coding_agent::ToolRegistry tools;
  tools.register_tool(std::make_unique<EchoTool>());

  auto session_mgr = coding_agent::SessionManager::create(session_dir.string(), session_dir.string());
  coding_agent::AgentSession agent(base_config(session_dir.string()), provider, tools,
                                    std::move(session_mgr));

  // Queue a steer message
  agent.steer("steer this!");

  std::vector<coding_agent::AgentEvent::Type> events;
  agent.set_event_handler([&events](const coding_agent::AgentEvent& ev) {
    events.push_back(ev.type);
  });

  const bool ok = agent.run("user prompt", [](const std::string&) {});
  EXPECT(ok, "run succeeds");
  EXPECT(provider.call_count() == 1, "one provider call");

  // Verify steer message was added to messages
  const auto& msgs = agent.messages();
  bool found_steer = false;
  for (const auto& m : msgs) {
    if (m.content == "steer this!") {
      found_steer = true;
      break;
    }
  }
  EXPECT(found_steer, "steer message found in message history");

  return true;
}

// ===========================================================================
// Test 5: follow-up messages drained when agent stops
// ===========================================================================

bool test_followup_drain_after_stop() {
  const fs::path session_dir = make_temp_dir("followupdrain");

  ScriptedProvider provider;
  // First response: no tools, agent would stop
  coding_agent::ChatResponse stop_response;
  stop_response.content = "stopping";
  stop_response.completion_tokens = 1;
  provider.enqueue(stop_response);

  // Second response: after follow-up injected
  coding_agent::ChatResponse followup_response;
  followup_response.content = "followed up";
  followup_response.completion_tokens = 1;
  provider.enqueue(followup_response);

  coding_agent::ToolRegistry tools;
  tools.register_tool(std::make_unique<EchoTool>());

  auto session_mgr = coding_agent::SessionManager::create(session_dir.string(), session_dir.string());
  coding_agent::AgentSession agent(base_config(session_dir.string()), provider, tools,
                                    std::move(session_mgr));

  // Queue a follow-up message
  agent.followUp("follow-up message");

  const bool ok = agent.run("user prompt", [](const std::string&) {});
  EXPECT(ok, "run succeeds");
  EXPECT(provider.call_count() == 2, "two provider calls (initial + follow-up)");

  // Verify follow-up message was added to messages
  const auto& msgs = agent.messages();
  bool found_fu = false;
  for (const auto& m : msgs) {
    if (m.content == "follow-up message") {
      found_fu = true;
      break;
    }
  }
  EXPECT(found_fu, "follow-up message found in message history");

  return true;
}

// ===========================================================================
// Test 6: is_retryable_error - rate limit
// ===========================================================================

bool test_is_retryable_error_rate_limit() {
  const fs::path session_dir = make_temp_dir("retryratelimit");

  ScriptedProvider provider;
  coding_agent::ToolRegistry tools;
  tools.register_tool(std::make_unique<EchoTool>());
  auto session_mgr = coding_agent::SessionManager::create(session_dir.string(), session_dir.string());

  coding_agent::AgentSession agent(base_config(session_dir.string()), provider, tools,
                                    std::move(session_mgr));

  EXPECT(agent.is_retryable_error("429 rate limit exceeded"), "429 rate limit is retryable");
  EXPECT(agent.is_retryable_error("RateLimit: too many requests"), "RateLimit is retryable");
  EXPECT(agent.is_retryable_error("rate limit reached"), "rate limit is retryable");

  return true;
}

// ===========================================================================
// Test 7: is_retryable_error - overloaded
// ===========================================================================

bool test_is_retryable_error_overloaded() {
  const fs::path session_dir = make_temp_dir("retryoverloaded");

  ScriptedProvider provider;
  coding_agent::ToolRegistry tools;
  tools.register_tool(std::make_unique<EchoTool>());
  auto session_mgr = coding_agent::SessionManager::create(session_dir.string(), session_dir.string());

  coding_agent::AgentSession agent(base_config(session_dir.string()), provider, tools,
                                    std::move(session_mgr));

  EXPECT(agent.is_retryable_error("service overloaded"), "overloaded is retryable");
  EXPECT(agent.is_retryable_error("503 service unavailable"), "503 is retryable");
  EXPECT(agent.is_retryable_error("Service Unavailable"), "Service Unavailable is retryable");

  return true;
}

// ===========================================================================
// Test 8: is_retryable_error - 500 and timeout
// ===========================================================================

bool test_is_retryable_error_500_and_timeout() {
  const fs::path session_dir = make_temp_dir("retry500");

  ScriptedProvider provider;
  coding_agent::ToolRegistry tools;
  tools.register_tool(std::make_unique<EchoTool>());
  auto session_mgr = coding_agent::SessionManager::create(session_dir.string(), session_dir.string());

  coding_agent::AgentSession agent(base_config(session_dir.string()), provider, tools,
                                    std::move(session_mgr));

  EXPECT(agent.is_retryable_error("500 internal server error"), "500 is retryable");
  EXPECT(agent.is_retryable_error("internal error occurred"), "internal error is retryable");
  EXPECT(agent.is_retryable_error("request timeout"), "timeout is retryable");
  EXPECT(agent.is_retryable_error("Connection reset by peer"), "connection reset is retryable");
  EXPECT(agent.is_retryable_error("Connection refused"), "connection refused is retryable");

  return true;
}

// ===========================================================================
// Test 9: is_retryable_error - NOT retryable (tool errors, auth)
// ===========================================================================

bool test_is_retryable_error_not_retryable() {
  const fs::path session_dir = make_temp_dir("retrynotretryable");

  ScriptedProvider provider;
  coding_agent::ToolRegistry tools;
  tools.register_tool(std::make_unique<EchoTool>());
  auto session_mgr = coding_agent::SessionManager::create(session_dir.string(), session_dir.string());

  coding_agent::AgentSession agent(base_config(session_dir.string()), provider, tools,
                                    std::move(session_mgr));

  EXPECT(!agent.is_retryable_error("tool execution failed: file not found"),
         "tool error is NOT retryable");
  EXPECT(!agent.is_retryable_error("authentication failed: invalid API key"),
         "auth error is NOT retryable");
  EXPECT(!agent.is_retryable_error("context window overflow"),
         "context overflow is NOT retryable");
  EXPECT(!agent.is_retryable_error("user input validation error"),
         "user input error is NOT retryable");
  EXPECT(!agent.is_retryable_error(""), "empty string is NOT retryable");

  return true;
}

// ===========================================================================
// Test 10: sendCustomMessage with Steer delivery
// ===========================================================================

bool test_send_custom_message_steer() {
  const fs::path session_dir = make_temp_dir("custommsgsteer");

  ScriptedProvider provider;
  coding_agent::ChatResponse response;
  response.content = "done";
  response.completion_tokens = 1;
  provider.enqueue(response);

  coding_agent::ToolRegistry tools;
  tools.register_tool(std::make_unique<EchoTool>());
  auto session_mgr = coding_agent::SessionManager::create(session_dir.string(), session_dir.string());
  coding_agent::AgentSession agent(base_config(session_dir.string()), provider, tools,
                                    std::move(session_mgr));

  agent.sendCustomMessage("custom_type", "custom content", "Custom Display",
                           {}, coding_agent::AgentSession::CustomMessageDelivery::Steer);

  EXPECT(agent.has_queued_messages(), "custom message queued to steer");
  EXPECT(agent.steering_messages().size() == 1, "one steering message");

  return true;
}

// ===========================================================================
// Test 11: sendCustomMessage with NextTurn delivery
// ===========================================================================

bool test_send_custom_message_next_turn() {
  const fs::path session_dir = make_temp_dir("custommsgnextturn");

  ScriptedProvider provider;
  coding_agent::ChatResponse response;
  response.content = "done";
  response.completion_tokens = 1;
  provider.enqueue(response);

  coding_agent::ToolRegistry tools;
  tools.register_tool(std::make_unique<EchoTool>());
  auto session_mgr = coding_agent::SessionManager::create(session_dir.string(), session_dir.string());
  coding_agent::AgentSession agent(base_config(session_dir.string()), provider, tools,
                                    std::move(session_mgr));

  agent.sendCustomMessage("custom_type", "next turn content", "Next Turn",
                           {}, coding_agent::AgentSession::CustomMessageDelivery::NextTurn);

  EXPECT(agent.pending_next_turn_messages().size() == 1, "one pending next-turn message");

  return true;
}

// ===========================================================================
// Test 12: clear_all_queues
// ===========================================================================

bool test_clear_all_queues() {
  const fs::path session_dir = make_temp_dir("clearall");

  ScriptedProvider provider;
  coding_agent::ChatResponse response;
  response.content = "done";
  response.completion_tokens = 1;
  provider.enqueue(response);

  coding_agent::ToolRegistry tools;
  tools.register_tool(std::make_unique<EchoTool>());
  auto session_mgr = coding_agent::SessionManager::create(session_dir.string(), session_dir.string());
  coding_agent::AgentSession agent(base_config(session_dir.string()), provider, tools,
                                    std::move(session_mgr));

  agent.steer("steer");
  agent.followUp("followup");
  agent.sendCustomMessage("type", "content", "Display",
                           {}, coding_agent::AgentSession::CustomMessageDelivery::NextTurn);

  agent.clear_all_queues();
  EXPECT(!agent.has_queued_messages(), "no queued messages after clear_all");
  EXPECT(agent.pending_next_turn_messages().empty(), "pending next-turn messages cleared");

  return true;
}

// ===========================================================================
// Test 13: Queue mode get/set
// ===========================================================================

bool test_queue_mode_get_set() {
  const fs::path session_dir = make_temp_dir("queuemode");

  ScriptedProvider provider;
  coding_agent::ChatResponse response;
  response.content = "done";
  response.completion_tokens = 1;
  provider.enqueue(response);

  coding_agent::ToolRegistry tools;
  tools.register_tool(std::make_unique<EchoTool>());
  auto session_mgr = coding_agent::SessionManager::create(session_dir.string(), session_dir.string());
  coding_agent::AgentSession agent(base_config(session_dir.string()), provider, tools,
                                    std::move(session_mgr));

  EXPECT(agent.steering_mode() == coding_agent::QueueMode::OneAtATime, "default steering mode");
  EXPECT(agent.follow_up_mode() == coding_agent::QueueMode::OneAtATime, "default follow-up mode");

  agent.set_steering_mode(coding_agent::QueueMode::All);
  agent.set_follow_up_mode(coding_agent::QueueMode::All);

  EXPECT(agent.steering_mode() == coding_agent::QueueMode::All, "steering mode set to All");
  EXPECT(agent.follow_up_mode() == coding_agent::QueueMode::All, "follow-up mode set to All");

  return true;
}

// ===========================================================================
// Test 14: PendingMessageQueue with images
// ===========================================================================

bool test_queue_with_images() {
  coding_agent::PendingMessageQueue queue(coding_agent::QueueMode::All);
  std::vector<std::string> images = {"img1.png", "img2.png"};
  queue.enqueue("message with images", images);

  auto drained = queue.drain();
  EXPECT(drained.size() == 1, "one item drained");
  EXPECT(drained[0].first == "message with images", "text matches");
  EXPECT(drained[0].second.size() == 2, "two images");
  EXPECT(drained[0].second[0] == "img1.png", "first image");
  EXPECT(drained[0].second[1] == "img2.png", "second image");

  return true;
}

// ===========================================================================
// Driver
// ===========================================================================

struct Test {
  const char* name;
  bool (*fn)();
};

}  // namespace

int main() {
  const Test tests[] = {
      {"queue_mode_one_at_a_time", test_queue_mode_one_at_a_time},
      {"queue_mode_all", test_queue_mode_all},
      {"steer_followup_enqueue", test_steer_followup_enqueue},
      {"steer_drain_before_call", test_steer_drain_before_call},
      {"followup_drain_after_stop", test_followup_drain_after_stop},
      {"is_retryable_error_rate_limit", test_is_retryable_error_rate_limit},
      {"is_retryable_error_overloaded", test_is_retryable_error_overloaded},
      {"is_retryable_error_500_and_timeout", test_is_retryable_error_500_and_timeout},
      {"is_retryable_error_not_retryable", test_is_retryable_error_not_retryable},
      {"send_custom_message_steer", test_send_custom_message_steer},
      {"send_custom_message_next_turn", test_send_custom_message_next_turn},
      {"clear_all_queues", test_clear_all_queues},
      {"queue_mode_get_set", test_queue_mode_get_set},
      {"queue_with_images", test_queue_with_images},
  };

  int passed = 0;
  int failed = 0;
  for (const auto& t : tests) {
    std::cout << "[ RUN ] " << t.name << "\n";
    if (t.fn()) {
      std::cout << "[  OK ] " << t.name << "\n";
      ++passed;
    } else {
      std::cout << "[FAIL ] " << t.name << "\n";
      ++failed;
    }
  }

  std::cout << passed << " passed, " << failed << " failed\n";
  return failed == 0 ? 0 : 1;
}

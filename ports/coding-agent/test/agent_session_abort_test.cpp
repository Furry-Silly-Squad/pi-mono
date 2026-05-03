// AgentSession tests: cancel flag, abort_requested_, emit_abort_event (issues 9, 14).
//
// Covers:
//  * cancel() sets interrupted_ and writes to the active cancel flag.
//  * abort_requested_ stops the run loop between tool iterations.
//  * abort() emits AgentEvent::Type::Abort and clears the flag.
//  * Multiple sequential runs after abort are unaffected.

#include "agent_session.hpp"
#include "providers/provider.hpp"
#include "session_entry.hpp"
#include "tools/tool_registry.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

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
// CancelableProvider: tracks cancel() calls and supports per-call cancellation
// ===========================================================================

class CancelableProvider final : public coding_agent::Provider {
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
    if (cancel_flag != nullptr && cancel_flag->load(std::memory_order_acquire)) {
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

  void cancel() override {
    ++cancel_count_;
    interrupted_ = true;
  }

  int call_count() const { return call_count_; }
  int cancel_count() const { return cancel_count_; }
  bool was_interrupted() const { return interrupted_; }
  size_t remaining() const { return responses_.size(); }

 private:
  std::queue<coding_agent::ChatResponse> responses_;
  int call_count_ = 0;
  int cancel_count_ = 0;
  bool interrupted_ = false;
};

// ===========================================================================
// BlockingProvider: first N chat() calls return queued responses immediately;
// subsequent chat() blocks until release() or cancel().
// ===========================================================================

class BlockingProvider final : public coding_agent::Provider {
 public:
  // no_wait_chats: e.g. 1 = first model response returns without blocking, second
  // chat blocks (tool-loop / multi-iteration tests). 0 = first chat blocks
  // immediately (abort-during-model-call tests).
  explicit BlockingProvider(int no_wait_chats = 0) : no_wait_chats_(no_wait_chats) {}

  void enqueue_response(coding_agent::ChatResponse response) {
    queue_.push(std::move(response));
  }

  void set_response(coding_agent::ChatResponse response) {
    enqueue_response(std::move(response));
  }

  bool chat(
      const coding_agent::ChatRequest& /*request*/,
      coding_agent::ChatResponse& response,
      const coding_agent::ChunkCallback& /*on_chunk*/,
      std::string& error,
      std::atomic<bool>* cancel_flag = nullptr
  ) override {
    ++call_count_;
    if (cancel_flag != nullptr && cancel_flag->load(std::memory_order_acquire)) {
      error = "interrupted";
      return false;
    }

    if (no_wait_done_ < no_wait_chats_) {
      if (queue_.empty()) {
        error = "no scripted response";
        return false;
      }
      response = queue_.front();
      queue_.pop();
      ++no_wait_done_;
      return true;
    }

    std::unique_lock<std::mutex> lock(mu_);
    cv_.wait(lock, [this] { return released_ || interrupted_.load(); });
    if (interrupted_.load()) {
      error = "interrupted";
      return false;
    }
    if (queue_.empty()) {
      error = "no scripted response after release";
      return false;
    }
    response = queue_.front();
    queue_.pop();
    return true;
  }

  void cancel() override {
    ++cancel_count_;
    interrupted_.store(true, std::memory_order_release);
    cv_.notify_all();
  }

  void release() {
    released_ = true;
    interrupted_.store(false, std::memory_order_release);
    no_wait_done_ = 0;
    cv_.notify_all();
  }

  int call_count() const { return call_count_; }
  int cancel_count() const { return cancel_count_; }
  size_t remaining() const { return queue_.size(); }

 private:
  const int no_wait_chats_;
  int no_wait_done_ = 0;
  std::queue<coding_agent::ChatResponse> queue_;
  std::atomic<int> call_count_{0};
  std::atomic<int> cancel_count_{0};
  std::atomic<bool> interrupted_{false};
  bool released_ = false;
  std::mutex mu_;
  std::condition_variable cv_;
};

// ===========================================================================
// Fake tool: counts invocations
// ===========================================================================

class CountingTool final : public coding_agent::Tool {
 public:
  std::string name() const override { return "count"; }
  std::string description() const override { return "counts invocations"; }
  std::string parameters_schema() const override { return "{}"; }
  coding_agent::ToolResult execute(const std::string& /*args_json*/,
                                   const std::string& /*cwd*/) override {
    ++call_count_;
    return coding_agent::ToolResult{.ok = true, .content = "counted"};
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
                 ("coding-agent-agent-session-abort-test-" +
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
  cfg.retry_enabled = false;
  return cfg;
}

bool test_cancel_stops_provider_call() {
  const fs::path session_dir = make_temp_dir("cancel_provider");

  CancelableProvider provider;

  coding_agent::ToolRegistry tools;
  tools.register_tool(std::make_unique<CountingTool>());

  auto session_mgr = coding_agent::SessionManager::create(session_dir.string(), session_dir.string());
  coding_agent::AgentSession agent(base_config(session_dir.string()), provider, tools,
                                    std::move(session_mgr));

  provider.cancel();
  EXPECT(provider.was_interrupted(), "provider marks itself as interrupted");

  std::atomic<bool> cancel_flag{false};
  const bool ok = agent.run("test", [](const std::string&) {}, &cancel_flag);
  EXPECT(!ok, "run fails when provider returns interrupted");
  EXPECT(provider.call_count() == 1, "provider was invoked once");

  return true;
}

bool test_abort_stops_between_tool_iterations() {
  const fs::path session_dir = make_temp_dir("abort_between");

  BlockingProvider provider(1);

  coding_agent::ChatResponse with_tool;
  with_tool.tool_calls.push_back(coding_agent::ToolCall{
      .id = "call_1",
      .name = "count",
      .arguments_json = "{}",
  });
  with_tool.completion_tokens = 5;
  provider.set_response(std::move(with_tool));

  coding_agent::ChatResponse with_tool2;
  with_tool2.tool_calls.push_back(coding_agent::ToolCall{
      .id = "call_2",
      .name = "count",
      .arguments_json = "{}",
  });
  with_tool2.completion_tokens = 5;
  provider.set_response(std::move(with_tool2));

  coding_agent::ToolRegistry tools;
  auto count_tool = std::make_unique<CountingTool>();
  CountingTool* count = count_tool.get();
  tools.register_tool(std::move(count_tool));

  auto session_mgr = coding_agent::SessionManager::create(session_dir.string(), session_dir.string());
  coding_agent::AgentSession agent(base_config(session_dir.string()), provider, tools,
                                    std::move(session_mgr));

  std::vector<coding_agent::AgentEvent::Type> events;
  agent.set_event_handler([&events](const coding_agent::AgentEvent& ev) {
    events.push_back(ev.type);
  });

  std::thread runner([&]() {
    agent.run("multi-step", [](const std::string&) {});
  });

  while (count->call_count() == 0) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }

  agent.abort();

  runner.join();

  EXPECT(!agent.is_running(), "agent is no longer running");
  EXPECT(count->call_count() == 1, "first tool call completed before abort");
  EXPECT(provider.call_count() == 2, "two provider calls (first tool iteration + blocked second)");

  bool saw_abort = false;
  for (const auto& t : events) {
    if (t == coding_agent::AgentEvent::Type::Abort) saw_abort = true;
  }
  EXPECT(saw_abort, "Abort event emitted after agent.abort()");

  return true;
}

bool test_abort_emits_abort_event() {
  const fs::path session_dir = make_temp_dir("abort_event");

  BlockingProvider provider(1);

  coding_agent::ChatResponse with_tool;
  with_tool.tool_calls.push_back(coding_agent::ToolCall{
      .id = "call_1",
      .name = "count",
      .arguments_json = "{}",
  });
  with_tool.completion_tokens = 5;
  provider.set_response(std::move(with_tool));

  coding_agent::ChatResponse with_tool2;
  with_tool2.tool_calls.push_back(coding_agent::ToolCall{
      .id = "call_2",
      .name = "count",
      .arguments_json = "{}",
  });
  with_tool2.completion_tokens = 5;
  provider.set_response(std::move(with_tool2));

  coding_agent::ToolRegistry tools;
  tools.register_tool(std::make_unique<CountingTool>());

  auto session_mgr = coding_agent::SessionManager::create(session_dir.string(), session_dir.string());
  coding_agent::AgentSession agent(base_config(session_dir.string()), provider, tools,
                                    std::move(session_mgr));

  std::vector<coding_agent::AgentEvent::Type> events;
  agent.set_event_handler([&events](const coding_agent::AgentEvent& ev) {
    events.push_back(ev.type);
  });

  std::thread runner([&]() {
    agent.run("test", [](const std::string&) {});
  });

  while (true) {
    const auto& msgs = agent.messages();
    if (msgs.size() >= 4) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }

  agent.abort();

  runner.join();

  int abort_count = 0;
  for (const auto& t : events) {
    if (t == coding_agent::AgentEvent::Type::Abort) ++abort_count;
  }
  EXPECT(abort_count == 1, "exactly one Abort event emitted");

  return true;
}

bool test_abort_flag_cleared() {
  const fs::path session_dir = make_temp_dir("abort_flag");

  BlockingProvider provider(1);

  coding_agent::ChatResponse with_tool;
  with_tool.tool_calls.push_back(coding_agent::ToolCall{
      .id = "call_1",
      .name = "count",
      .arguments_json = "{}",
  });
  with_tool.completion_tokens = 5;
  provider.set_response(std::move(with_tool));

  coding_agent::ChatResponse with_tool2;
  with_tool2.tool_calls.push_back(coding_agent::ToolCall{
      .id = "call_2",
      .name = "count",
      .arguments_json = "{}",
  });
  with_tool2.completion_tokens = 5;
  provider.set_response(std::move(with_tool2));

  coding_agent::ToolRegistry tools;
  tools.register_tool(std::make_unique<CountingTool>());

  auto session_mgr = coding_agent::SessionManager::create(session_dir.string(), session_dir.string());
  coding_agent::AgentSession agent(base_config(session_dir.string()), provider, tools,
                                    std::move(session_mgr));

  std::thread runner([&]() {
    agent.run("first", [](const std::string&) {});
  });

  while (true) {
    const auto& msgs = agent.messages();
    if (msgs.size() >= 4) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }

  agent.abort();
  runner.join();

  provider.release();
  provider.set_response(coding_agent::ChatResponse{.content = "done", .completion_tokens = 1});
  const bool ok = agent.run("second", [](const std::string&) {});
  EXPECT(ok, "second run succeeds after abort was cleared");
  EXPECT(provider.call_count() >= 2, "provider called for both runs");

  return true;
}

bool test_abort_before_each_model_call() {
  const fs::path session_dir = make_temp_dir("abort_each_call");

  BlockingProvider provider(1);

  coding_agent::ChatResponse with_tool;
  with_tool.tool_calls.push_back(coding_agent::ToolCall{
      .id = "call_1",
      .name = "count",
      .arguments_json = "{}",
  });
  with_tool.completion_tokens = 5;
  provider.set_response(std::move(with_tool));

  coding_agent::ChatResponse with_tool2;
  with_tool2.tool_calls.push_back(coding_agent::ToolCall{
      .id = "call_2",
      .name = "count",
      .arguments_json = "{}",
  });
  with_tool2.completion_tokens = 5;
  provider.set_response(std::move(with_tool2));

  coding_agent::ToolRegistry tools;
  auto count_tool = std::make_unique<CountingTool>();
  CountingTool* count = count_tool.get();
  tools.register_tool(std::move(count_tool));

  auto session_mgr = coding_agent::SessionManager::create(session_dir.string(), session_dir.string());
  coding_agent::AgentSession agent(base_config(session_dir.string()), provider, tools,
                                    std::move(session_mgr));

  std::thread runner([&]() {
    agent.run("abort-test", [](const std::string&) {});
  });

  while (count->call_count() == 0) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }

  agent.abort();

  runner.join();

  EXPECT(count->call_count() == 1, "first tool call completed");
  EXPECT(provider.call_count() == 2, "provider called for first iteration and blocked second");

  return true;
}

bool test_abort_calls_provider_cancel() {
  const fs::path session_dir = make_temp_dir("abort_calls_cancel");

  BlockingProvider provider(0);
  provider.set_response(coding_agent::ChatResponse{.content = "done", .completion_tokens = 1});

  coding_agent::ToolRegistry tools;
  tools.register_tool(std::make_unique<CountingTool>());

  auto session_mgr = coding_agent::SessionManager::create(session_dir.string(), session_dir.string());
  coding_agent::AgentSession agent(base_config(session_dir.string()), provider, tools,
                                    std::move(session_mgr));

  std::thread runner([&]() {
    agent.run("test", [](const std::string&) {});
  });

  while (provider.call_count() == 0) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }

  agent.abort();

  runner.join();

  EXPECT(provider.cancel_count() >= 1, "provider.cancel() called by abort()");

  return true;
}

bool test_cancel_flag_and_abort_compatible() {
  const fs::path session_dir = make_temp_dir("cancel_abort_compat");

  CancelableProvider provider;
  coding_agent::ChatResponse response;
  response.content = "done";
  response.completion_tokens = 1;
  provider.enqueue(response);

  coding_agent::ToolRegistry tools;
  tools.register_tool(std::make_unique<CountingTool>());

  auto session_mgr = coding_agent::SessionManager::create(session_dir.string(), session_dir.string());
  coding_agent::AgentSession agent(base_config(session_dir.string()), provider, tools,
                                    std::move(session_mgr));

  std::atomic<bool> cancel_flag{true};

  const bool ok = agent.run("test", [](const std::string&) {}, &cancel_flag);
  EXPECT(!ok, "run fails when cancel_flag is set");
  EXPECT(provider.call_count() == 1, "provider invoked once");

  return true;
}

bool test_multiple_aborts_safe() {
  const fs::path session_dir = make_temp_dir("multiple_aborts");

  BlockingProvider provider(0);
  provider.set_response(coding_agent::ChatResponse{.content = "done", .completion_tokens = 1});

  coding_agent::ToolRegistry tools;
  tools.register_tool(std::make_unique<CountingTool>());

  auto session_mgr = coding_agent::SessionManager::create(session_dir.string(), session_dir.string());
  coding_agent::AgentSession agent(base_config(session_dir.string()), provider, tools,
                                    std::move(session_mgr));

  std::thread runner([&]() {
    agent.run("test", [](const std::string&) {});
  });

  while (provider.call_count() == 0) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }

  agent.abort();
  agent.abort();
  agent.abort();

  runner.join();

  EXPECT(provider.cancel_count() >= 1, "provider.cancel() called");

  return true;
}

struct Test {
  const char* name;
  bool (*fn)();
};

}  // namespace

int main() {
  const Test tests[] = {
      {"cancel_stops_provider_call", test_cancel_stops_provider_call},
      {"abort_stops_between_tool_iterations", test_abort_stops_between_tool_iterations},
      {"abort_emits_abort_event", test_abort_emits_abort_event},
      {"abort_flag_cleared", test_abort_flag_cleared},
      {"abort_before_each_model_call", test_abort_before_each_model_call},
      {"abort_calls_provider_cancel", test_abort_calls_provider_cancel},
      {"cancel_flag_and_abort_compatible", test_cancel_flag_and_abort_compatible},
      {"multiple_aborts_safe", test_multiple_aborts_safe},
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

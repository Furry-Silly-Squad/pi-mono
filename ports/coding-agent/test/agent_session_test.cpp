// High-impact unit tests for AgentSession (Phase 7).
//
// Covers:
//  * Constructor: history loading, system message creation + persistence.
//  * Single-turn run: user/assistant message append, event order
//    (TurnStart, ModelCallStart, TurnEnd).
//  * Tool-call flow: ModelCallStart → ToolCall → ToolResult → ModelCallStart →
//    TurnEnd, tool message append between iterations.
//  * Active tool filtering via initial_active_tools.
//  * Auto-compaction (threshold-driven) emits CompactionStart/End and persists
//    a compaction row.
//  * Manual compact() succeeds and emits events.
//  * Interrupt (cancel_flag) rolls back the user message.
//  * ThinkingLevel: round-trip + cycle (incl. XHigh) + ThinkingLevelChange event.
//  * set_model emits ModelChange and updates state.
//  * Reload across instances preserves history.

#include "agent_session.hpp"

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
#include "session.hpp"
#include "tools/tool.hpp"
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
// Fake tool: returns a fixed result, records invocations
// ===========================================================================

class EchoTool final : public coding_agent::Tool {
 public:
  std::string name() const override { return "echo"; }
  std::string description() const override { return "echo back the input"; }
  std::string parameters_schema() const override {
    return R"({"type":"object","properties":{"text":{"type":"string"}}})";
  }
  coding_agent::ToolResult execute(const std::string& args_json,
                                   const std::string& /*cwd*/) override {
    last_args_ = args_json;
    ++call_count_;
    return coding_agent::ToolResult{.ok = true, .content = "echoed"};
  }
  int call_count() const { return call_count_; }
  const std::string& last_args() const { return last_args_; }

 private:
  int call_count_ = 0;
  std::string last_args_;
};

class NoopTool final : public coding_agent::Tool {
 public:
  std::string name() const override { return "noop"; }
  std::string description() const override { return "noop"; }
  std::string parameters_schema() const override { return "{}"; }
  coding_agent::ToolResult execute(const std::string& /*args_json*/,
                                   const std::string& /*cwd*/) override {
    return coding_agent::ToolResult{.ok = true, .content = "noop"};
  }
};

// ===========================================================================
// Helpers
// ===========================================================================

namespace fs = std::filesystem;

fs::path make_temp_dir(const std::string& tag) {
  static int counter = 0;
  ++counter;
  fs::path dir = fs::temp_directory_path() /
                 ("coding-agent-agent-session-test-" +
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
  cfg.auto_compaction = true;
  cfg.initial_active_tools = "echo";
  return cfg;
}

std::string pad_chars(int approx_tokens) {
  return std::string(static_cast<size_t>(std::max(1, approx_tokens) * 4), 'x');
}

// ===========================================================================
// Test 1: Lifecycle (constructor, single turn, persistence, reload)
// ===========================================================================

bool test_lifecycle() {
  const fs::path session_dir = make_temp_dir("lifecycle");

  ScriptedProvider provider;
  coding_agent::ChatResponse first;
  first.content = "hello back";
  first.completion_tokens = 5;
  provider.enqueue(first);

  coding_agent::ToolRegistry tools;
  tools.register_tool(std::make_unique<EchoTool>());

  coding_agent::SessionStore session(".", session_dir.string());
  session.start_or_resume(std::nullopt, true);

  std::vector<coding_agent::AgentEvent::Type> event_types;
  std::string captured_chunks;

  {
    coding_agent::AgentSession agent(base_config(session_dir.string()), provider, tools, session);
    agent.set_event_handler([&event_types](const coding_agent::AgentEvent& ev) {
      event_types.push_back(ev.type);
    });

    EXPECT(agent.message_count() == 1, "constructor must persist exactly the system message");
    EXPECT(agent.messages()[0].role == "system", "first message must be system");
    EXPECT(!agent.system_prompt().empty(), "system prompt must be populated");
    EXPECT(agent.model() == "fake-model", "initial model");
    EXPECT(agent.thinking_level() == coding_agent::ThinkingLevel::Off, "initial thinking level");
    EXPECT(!agent.session_id().empty(), "session id assigned");

    const bool ok = agent.run("hello", [&captured_chunks](const std::string& s) {
      captured_chunks += s;
    });
    EXPECT(ok, "run() must succeed");
    EXPECT(provider.call_count() == 1, "exactly one provider call");
    EXPECT(agent.message_count() == 3, "system + user + assistant");
    EXPECT(agent.messages()[1].role == "user" && agent.messages()[1].content == "hello",
           "user message recorded");
    EXPECT(agent.messages()[2].role == "assistant" && agent.messages()[2].content == "hello back",
           "assistant message recorded");
    EXPECT(captured_chunks.find("→") != std::string::npos,
           "per-turn token breakdown must be streamed");
  }

  // Verify event order: TurnStart, ModelCallStart, TurnEnd.
  EXPECT(event_types.size() == 3, "expected 3 events for tool-less turn");
  EXPECT(event_types[0] == coding_agent::AgentEvent::Type::TurnStart, "first event = TurnStart");
  EXPECT(event_types[1] == coding_agent::AgentEvent::Type::ModelCallStart, "second event = ModelCallStart");
  EXPECT(event_types[2] == coding_agent::AgentEvent::Type::TurnEnd, "third event = TurnEnd");

  // Reload via fresh AgentSession on the same dir/id and verify history is preserved.
  ScriptedProvider provider2;
  coding_agent::SessionStore session2(".", session_dir.string());
  session2.start_or_resume(session.get_session_id(), false);
  coding_agent::AgentSession agent2(base_config(session_dir.string()), provider2, tools, session2);
  EXPECT(agent2.message_count() == 3, "reloaded history preserved (system + user + assistant)");
  EXPECT(agent2.messages()[2].content == "hello back", "reloaded assistant content matches");

  return true;
}

// ===========================================================================
// Test 2: Tool-call flow + active tool filtering
// ===========================================================================

bool test_tool_flow() {
  const fs::path session_dir = make_temp_dir("toolflow");

  ScriptedProvider provider;
  // First response: assistant requests tool.
  coding_agent::ChatResponse with_tool;
  with_tool.tool_calls.push_back(coding_agent::ToolCall{
      .id = "call_1",
      .name = "echo",
      .arguments_json = R"({"text":"hi"})",
  });
  with_tool.completion_tokens = 8;
  provider.enqueue(with_tool);

  // Second response: assistant answers without tools.
  coding_agent::ChatResponse done;
  done.content = "ok done";
  done.completion_tokens = 3;
  provider.enqueue(done);

  coding_agent::ToolRegistry tools;
  auto echo_owned = std::make_unique<EchoTool>();
  EchoTool* echo = echo_owned.get();
  tools.register_tool(std::move(echo_owned));
  tools.register_tool(std::make_unique<NoopTool>());

  coding_agent::SessionStore session(".", session_dir.string());
  session.start_or_resume(std::nullopt, true);

  auto cfg = base_config(session_dir.string());
  cfg.initial_active_tools = "echo";  // restrict to echo only
  coding_agent::AgentSession agent(cfg, provider, tools, session);

  EXPECT(agent.active_tools().size() == 1 && agent.active_tools().front() == "echo",
         "active_tools should reflect initial_active_tools filter");

  std::vector<coding_agent::AgentEvent::Type> events;
  std::string last_tool_name;
  agent.set_event_handler([&events, &last_tool_name](const coding_agent::AgentEvent& ev) {
    events.push_back(ev.type);
    if (ev.type == coding_agent::AgentEvent::Type::ToolCall ||
        ev.type == coding_agent::AgentEvent::Type::ToolResult) {
      last_tool_name = ev.tool_name;
    }
  });

  const bool ok = agent.run("please echo", [](const std::string&) {});
  EXPECT(ok, "run with tool should succeed");
  EXPECT(provider.call_count() == 2, "two provider calls (with-tool, then final)");
  EXPECT(echo->call_count() == 1, "echo tool dispatched once");
  EXPECT(echo->last_args().find("\"hi\"") != std::string::npos, "echo received text=hi");

  // system + user + assistant(call) + tool + assistant
  EXPECT(agent.message_count() == 5, "expected 5 messages after tool round");
  EXPECT(agent.messages()[2].role == "assistant" && !agent.messages()[2].tool_calls.empty(),
         "third message is assistant with tool_calls");
  EXPECT(agent.messages()[3].role == "tool", "fourth message is tool");
  EXPECT(agent.messages()[3].tool_call_id.has_value() &&
             agent.messages()[3].tool_call_id.value() == "call_1",
         "tool message references call id");
  EXPECT(agent.messages()[4].role == "assistant" && agent.messages()[4].content == "ok done",
         "fifth message is final assistant text");

  // Event order:
  //   TurnStart, ModelCallStart, ToolCall, ToolResult, ModelCallStart, TurnEnd
  EXPECT(events.size() == 6, "expected 6 events for one tool round + final answer");
  EXPECT(events[0] == coding_agent::AgentEvent::Type::TurnStart, "TurnStart");
  EXPECT(events[1] == coding_agent::AgentEvent::Type::ModelCallStart, "ModelCallStart #1");
  EXPECT(events[2] == coding_agent::AgentEvent::Type::ToolCall, "ToolCall");
  EXPECT(events[3] == coding_agent::AgentEvent::Type::ToolResult, "ToolResult");
  EXPECT(events[4] == coding_agent::AgentEvent::Type::ModelCallStart, "ModelCallStart #2");
  EXPECT(events[5] == coding_agent::AgentEvent::Type::TurnEnd, "TurnEnd");
  EXPECT(last_tool_name == "echo", "tool events name=echo");

  return true;
}

// ===========================================================================
// Test 3: Auto-compaction triggered by threshold
// ===========================================================================

bool test_auto_compaction() {
  const fs::path session_dir = make_temp_dir("autocompact");

  ScriptedProvider provider;

  // First model call: a small response that, combined with priming history,
  // pushes us across the threshold and triggers compaction.
  coding_agent::ChatResponse small_reply;
  small_reply.content = "small reply";
  small_reply.completion_tokens = 5;
  provider.enqueue(small_reply);

  // The compaction step calls the provider once for the summary.
  coding_agent::ChatResponse summary_reply;
  summary_reply.content = "CANNED_SUMMARY";
  summary_reply.completion_tokens = 10;
  provider.enqueue(summary_reply);

  coding_agent::ToolRegistry tools;
  tools.register_tool(std::make_unique<EchoTool>());

  coding_agent::SessionStore session(".", session_dir.string());
  session.start_or_resume(std::nullopt, true);

  // Pre-seed the session with bulky messages so total tokens already exceed
  // (context_size - reserve_tokens) once the next assistant reply is appended.
  std::string append_error;
  for (int i = 0; i < 6; ++i) {
    coding_agent::ChatMessage filler{
        .role = (i % 2 == 0 ? "user" : "assistant"),
        .content = pad_chars(200),
    };
    filler.entry_id = session.assign_entry_id();
    session.append(filler, append_error);
  }

  auto cfg = base_config(session_dir.string());
  cfg.context_size = 1024;
  cfg.compaction_reserve_tokens = 256;
  cfg.compaction_keep_recent_tokens = 64;
  cfg.max_tool_iterations = 1;  // single round

  coding_agent::AgentSession agent(cfg, provider, tools, session);
  EXPECT(agent.message_count() >= 6, "history pre-seeded");

  std::vector<coding_agent::AgentEvent::Type> events;
  agent.set_event_handler([&events](const coding_agent::AgentEvent& ev) {
    events.push_back(ev.type);
  });

  const int tokens_before_run = agent.total_context_tokens();
  EXPECT(tokens_before_run > (cfg.context_size - cfg.compaction_reserve_tokens),
         "fixture must already exceed compaction threshold (tune pad if not)");

  const bool ok = agent.run("trigger compaction", [](const std::string&) {});
  EXPECT(ok, "auto-compaction run must succeed");
  EXPECT(agent.compaction_count() == 1, "exactly one compaction recorded");

  bool saw_compaction_start = false;
  bool saw_compaction_end = false;
  for (const auto& t : events) {
    if (t == coding_agent::AgentEvent::Type::CompactionStart) saw_compaction_start = true;
    if (t == coding_agent::AgentEvent::Type::CompactionEnd) saw_compaction_end = true;
  }
  EXPECT(saw_compaction_start, "CompactionStart event emitted");
  EXPECT(saw_compaction_end, "CompactionEnd event emitted");

  EXPECT(agent.last_compaction_stats().did_compact, "stats.did_compact = true");
  EXPECT(agent.last_compaction_stats().summary.find("CANNED_SUMMARY") != std::string::npos,
         "summary contains provider output");

  return true;
}

// ===========================================================================
// Test 4: Manual compact() after a populated history
// ===========================================================================

bool test_manual_compact() {
  const fs::path session_dir = make_temp_dir("manualcompact");

  ScriptedProvider provider;
  coding_agent::ChatResponse summary_reply;
  summary_reply.content = "CANNED_SUMMARY";
  summary_reply.completion_tokens = 4;
  provider.enqueue(summary_reply);

  coding_agent::ToolRegistry tools;
  tools.register_tool(std::make_unique<EchoTool>());

  coding_agent::SessionStore session(".", session_dir.string());
  session.start_or_resume(std::nullopt, true);

  std::string append_error;
  for (int i = 0; i < 8; ++i) {
    coding_agent::ChatMessage filler{
        .role = (i % 2 == 0 ? "user" : "assistant"),
        .content = pad_chars(300),
    };
    filler.entry_id = session.assign_entry_id();
    session.append(filler, append_error);
  }

  auto cfg = base_config(session_dir.string());
  cfg.context_size = 4096;
  cfg.compaction_reserve_tokens = 512;
  cfg.compaction_keep_recent_tokens = 128;
  coding_agent::AgentSession agent(cfg, provider, tools, session);

  std::vector<coding_agent::AgentEvent::Type> events;
  agent.set_event_handler([&events](const coding_agent::AgentEvent& ev) {
    events.push_back(ev.type);
  });

  const bool compacted = agent.compact();
  EXPECT(compacted, "manual compact() must report success");
  EXPECT(agent.compaction_count() == 1, "compaction_count incremented");

  EXPECT(events.size() == 2, "manual compact emits exactly 2 events");
  EXPECT(events[0] == coding_agent::AgentEvent::Type::CompactionStart, "first = CompactionStart");
  EXPECT(events[1] == coding_agent::AgentEvent::Type::CompactionEnd, "second = CompactionEnd");

  return true;
}

// ===========================================================================
// Test 5: Interrupt rolls back the user message
// ===========================================================================

bool test_interrupt_rolls_back_user() {
  const fs::path session_dir = make_temp_dir("interrupt");

  ScriptedProvider provider;  // no responses queued

  coding_agent::ToolRegistry tools;
  tools.register_tool(std::make_unique<EchoTool>());

  coding_agent::SessionStore session(".", session_dir.string());
  session.start_or_resume(std::nullopt, true);

  coding_agent::AgentSession agent(base_config(session_dir.string()), provider, tools, session);
  const size_t before = agent.message_count();
  EXPECT(before == 1, "only system message present");

  std::atomic<bool> cancel_flag{true};
  const bool ok = agent.run("never replied", [](const std::string&) {}, &cancel_flag);
  EXPECT(!ok, "run must report failure on interrupt");
  EXPECT(agent.message_count() == before,
         "user message must be rolled back from in-memory history");

  // Provider must not have produced anything; call_count == 1 (one attempt that returned interrupted).
  EXPECT(provider.call_count() == 1, "provider was invoked exactly once before being interrupted");

  return true;
}

// ===========================================================================
// Test 6: ThinkingLevel + model change + cycling
// ===========================================================================

bool test_thinking_and_model() {
  const fs::path session_dir = make_temp_dir("thinking");

  ScriptedProvider provider;
  coding_agent::ToolRegistry tools;
  tools.register_tool(std::make_unique<EchoTool>());
  coding_agent::SessionStore session(".", session_dir.string());
  session.start_or_resume(std::nullopt, true);

  coding_agent::AgentSession agent(base_config(session_dir.string()), provider, tools, session);

  // Round-trip ThinkingLevel <-> string.
  using TL = coding_agent::ThinkingLevel;
  for (TL level : {TL::Off, TL::Minimal, TL::Low, TL::Medium, TL::High, TL::XHigh}) {
    const std::string s = coding_agent::thinking_level_to_string(level);
    EXPECT(coding_agent::string_to_thinking_level(s) == level,
           "thinking_level round-trip");
  }
  EXPECT(coding_agent::string_to_thinking_level("garbage") == TL::Off,
         "unknown string -> Off");

  std::vector<coding_agent::AgentEvent::Type> events;
  std::string last_model_change_to;
  TL last_thinking_change_to = TL::Off;
  agent.set_event_handler([&](const coding_agent::AgentEvent& ev) {
    events.push_back(ev.type);
    if (ev.type == coding_agent::AgentEvent::Type::ModelChange) {
      last_model_change_to = ev.new_model;
    } else if (ev.type == coding_agent::AgentEvent::Type::ThinkingLevelChange) {
      last_thinking_change_to = ev.new_thinking_level;
    }
  });

  // set_model: first call emits, second call (same id) is a no-op.
  EXPECT(agent.set_model("other-model"), "set_model returns true on change");
  EXPECT(agent.model() == "other-model", "model updated");
  EXPECT(!agent.set_model("other-model"), "set_model returns false when unchanged");
  EXPECT(last_model_change_to == "other-model", "ModelChange event payload");

  // Cycle thinking level forward through all 6 levels including XHigh.
  EXPECT(agent.thinking_level() == TL::Off, "starts at Off");
  std::vector<TL> seen;
  seen.push_back(agent.thinking_level());
  for (int i = 0; i < 6; ++i) {
    EXPECT(agent.cycle_thinking_level(true), "cycle returns true");
    seen.push_back(agent.thinking_level());
  }
  // After 6 forward cycles from Off we wrap back to Off.
  EXPECT(seen.front() == TL::Off && seen.back() == TL::Off,
         "6-step forward cycle wraps to Off");
  bool saw_xhigh = false;
  for (TL t : seen) {
    if (t == TL::XHigh) saw_xhigh = true;
  }
  EXPECT(saw_xhigh, "XHigh visited during cycle");
  EXPECT(last_thinking_change_to == TL::Off, "final ThinkingLevelChange wraps to Off");

  // ModelChange counted once, ThinkingLevelChange counted at least 6 times.
  int model_changes = 0;
  int thinking_changes = 0;
  for (auto t : events) {
    if (t == coding_agent::AgentEvent::Type::ModelChange) ++model_changes;
    if (t == coding_agent::AgentEvent::Type::ThinkingLevelChange) ++thinking_changes;
  }
  EXPECT(model_changes == 1, "ModelChange emitted exactly once");
  EXPECT(thinking_changes == 6, "ThinkingLevelChange emitted 6 times");

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
      {"lifecycle", test_lifecycle},
      {"tool_flow", test_tool_flow},
      {"auto_compaction", test_auto_compaction},
      {"manual_compact", test_manual_compact},
      {"interrupt_rolls_back_user", test_interrupt_rolls_back_user},
      {"thinking_and_model", test_thinking_and_model},
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

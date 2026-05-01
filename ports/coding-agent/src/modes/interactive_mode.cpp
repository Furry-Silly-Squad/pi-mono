#include "modes/interactive_mode.hpp"

#include <atomic>
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <sstream>
#include <string>

#include <readline/history.h>
#include <readline/readline.h>

#include "agent_loop.hpp"
#include "compaction.hpp"
#include "modes/tui_animation.hpp"

namespace coding_agent {
namespace {

// Global cancel flag for Ctrl+C handling
static std::atomic<bool> global_cancel_flag = false;

void signal_handler(int /*signum*/) {
  global_cancel_flag.store(true, std::memory_order_release);
}

// ANSI color codes
const char* COLOR_GREEN = "\033[0;32m";
const char* COLOR_YELLOW = "\033[0;33m";
const char* COLOR_RED = "\033[0;31m";
const char* COLOR_RESET = "\033[0m";

std::string format_token_budget_status(
    const std::vector<ChatMessage>& history,
    int context_size,
    int reserve_tokens
) {
  const int total_tokens = total_context_tokens(history);
  const int budget = context_size - reserve_tokens;
  const int remaining = std::max(0, budget - total_tokens);
  const double pct = budget > 0 ? (static_cast<double>(total_tokens) / budget) * 100.0 : 0.0;

  // Color coding: green (<50%), yellow (50-80%), red (>80%)
  const char* color = COLOR_GREEN;
  if (pct > 80.0) color = COLOR_RED;
  else if (pct > 50.0) color = COLOR_YELLOW;

  std::ostringstream oss;
  oss << color << "[" << static_cast<int>(pct) << "%]" << COLOR_RESET;
  oss << " " << total_tokens << " / " << budget << " tokens";

  if (remaining > 0) {
    oss << " | compaction in ~" << remaining << " tokens";
  } else {
    oss << " | compaction threshold reached";
  }

  return oss.str();
}

bool should_warn_compaction(
    const std::vector<ChatMessage>& history,
    int context_size,
    int reserve_tokens
) {
  const int total_tokens = total_context_tokens(history);
  const int budget = context_size - reserve_tokens;
  const int remaining = budget - total_tokens;
  // Warn if within 10% of budget (remaining < 10% of budget)
  return remaining < (budget * 0.1);
}

}  // namespace

int run_interactive_mode(
    const Config& config,
    Provider& provider,
    ToolRegistry& tools,
    std::vector<ChatMessage>& history,
    SessionStore& session
) {
  // Set up signal handler for Ctrl+C
  struct sigaction sa;
  sa.sa_handler = signal_handler;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = 0;
  sigaction(SIGINT, &sa, nullptr);

  global_cancel_flag.store(false, std::memory_order_release);

  while (true) {
    char* line = readline("> ");
    if (line == nullptr) {
      std::cout << "\n";
      return 0;
    }

    std::string prompt(line);
    free(line);
    if (prompt.empty()) {
      continue;
    }
    if (prompt == "/exit" || prompt == "/quit") {
      return 0;
    }
    if (prompt == "/clear") {
      history.clear();
      std::cout << "History cleared.\n";
      continue;
    }
    if (prompt == "/stats" || prompt == "/session") {
      std::cout << "\n=== Session Stats ===\n";
      std::cout << "Session ID: " << session.get_session_id() << "\n";
      std::cout << "Messages: " << history.size() << "\n";
      const int total_tokens = total_context_tokens(history);
      const int budget = config.context_size - config.compaction_reserve_tokens;
      std::cout << "Tokens: " << total_tokens << " / " << budget << " ("
                << (budget > 0 ? std::to_string(static_cast<int>(
                                     (static_cast<double>(total_tokens) / budget) * 100.0))
                                + "%"
                                : "N/A")
                << ")\n";
      const int remaining = std::max(0, budget - total_tokens);
      std::cout << "Compaction in ~" << remaining << " tokens\n";
      std::cout << "Compactions: " << session.get_compaction_count() << "\n";
      if (session.get_compaction_count() > 0) {
        const auto& last = session.get_last_compaction_event();
        std::cout << "Last compaction: " << last.tokens_before << " -> " << last.tokens_after << " tokens\n";
      }
      std::cout << "Context size: " << config.context_size << "\n";
      std::cout << "Reserve tokens: " << config.compaction_reserve_tokens << "\n";
      std::cout << "Keep recent tokens: " << config.compaction_keep_recent_tokens << "\n";
      std::cout << "=====================\n\n";
      continue;
    }
    if (prompt == "/tokens") {
      std::cout << "\nTokens: " << total_context_tokens(history) << " / "
                << (config.context_size - config.compaction_reserve_tokens) << "\n";
      std::cout << "=====================\n\n";
      continue;
    }

    // Reset cancel flag for new request
    global_cancel_flag.store(false, std::memory_order_release);

    // Start TUI animation while waiting for LLM
    TuiAnimation animation;
    animation.start(AnimationState::Thinking, "");

    add_history(prompt.c_str());
    const RunResult result =
        run_agent_loop(config, provider, tools, history, session, prompt, [&animation](const std::string& chunk) {
          // Switch to generating state on first chunk
          animation.update(AnimationState::Generating, "");
          std::cout << chunk << std::flush;
        }, &global_cancel_flag);

    // Stop TUI animation
    animation.stop();

    // Print compaction proximity warning if needed
    if (should_warn_compaction(history, config.context_size, config.compaction_reserve_tokens)) {
      std::cout << "\n[WARN] compaction in ~"
                << (config.context_size - config.compaction_reserve_tokens) - total_context_tokens(history)
                << " tokens\n";
    }

    if (!result.ok) {
      if (result.error == "interrupted") {
        std::cout << "\n[interrupted]\n";
      } else {
        std::cerr << "\nError: " << result.error << "\n";
      }
    } else {
      std::cout << "\n";
    }

    // Print token budget status line after each response
    std::cout << format_token_budget_status(history, config.context_size, config.compaction_reserve_tokens)
              << "\n";
  }
}

}  // namespace coding_agent

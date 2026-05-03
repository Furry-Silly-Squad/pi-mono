#include "modes/interactive_mode.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <csignal>
#include <iostream>
#include <sstream>
#include <string>

#include <readline/history.h>
#include <readline/readline.h>

#include "agent_session.hpp"
#include "compaction.hpp"
#include "modes/tui_animation.hpp"

namespace coding_agent {
namespace {

static std::atomic<bool> global_cancel_flag = false;

void signal_handler(int /*signum*/) {
    global_cancel_flag.store(true, std::memory_order_release);
}

const char* COLOR_GREEN  = "\033[0;32m";
const char* COLOR_YELLOW = "\033[0;33m";
const char* COLOR_RED    = "\033[0;31m";
const char* COLOR_RESET  = "\033[0m";

std::string format_token_budget(const AgentSession& agent) {
    const auto& cfg    = agent.session_config();
    const int total    = agent.total_context_tokens();
    const int budget   = cfg.context_size - cfg.compaction_reserve_tokens;
    const int remaining = std::max(0, budget - total);
    const double pct   = budget > 0 ? (static_cast<double>(total) / budget) * 100.0 : 0.0;

    const char* color = COLOR_GREEN;
    if (pct > 80.0) color = COLOR_RED;
    else if (pct > 50.0) color = COLOR_YELLOW;

    std::ostringstream oss;
    oss << color << "[" << static_cast<int>(pct) << "%]" << COLOR_RESET
        << " " << total << " / " << budget << " tokens"
        << " | compaction in ~" << remaining << " tokens";
    return oss.str();
}

bool near_compaction_threshold(const AgentSession& agent) {
    const auto& cfg  = agent.session_config();
    const int total  = agent.total_context_tokens();
    const int budget = cfg.context_size - cfg.compaction_reserve_tokens;
    return (budget - total) < (budget / 10);
}

void print_interactive_turn_debug(const AgentSession& agent) {
    const TurnDebugInfo& d = agent.last_turn_debug();
    const auto& cfg       = agent.session_config();
    std::cout << "--- turn debug ---\n";
    std::cout << "model_rounds: " << d.model_rounds << "\n";
    std::cout << "max_tool_iterations (config): " << cfg.max_tool_iterations << "\n";
    std::cout << "hit_max_tool_iterations: " << (d.hit_max_tool_iterations ? "yes" : "no") << "\n";
    if (!d.trailing_message_role.empty()) {
        std::cout << "trailing_message_role: " << d.trailing_message_role << "\n";
    }
    std::cout << "final_assistant_chars: " << d.final_assistant_content_chars << "\n";
    std::cout << "final_assistant_tool_calls: " << d.final_assistant_tool_call_count << "\n";
    if (d.final_assistant_content_chars > 0) {
        std::cout << "assistant_tail_last_~20_tokens: " << d.final_assistant_tail_esc << "\n";
    } else if (d.model_rounds > 0) {
        std::cout << "assistant_tail_last_~20_tokens: (empty assistant content)\n";
    }
    if (!d.run_failure_kind.empty()) {
        std::cout << "run_failure_kind: " << d.run_failure_kind << "\n";
        if (!d.provider_error.empty()) {
            std::cout << "provider_error: " << d.provider_error << "\n";
        }
    }
    std::cout << "---\n";
}

}  // namespace

int run_interactive_mode(AgentSession& agent, bool interactive_debug) {
    struct sigaction sa{};
    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, nullptr);

    global_cancel_flag.store(false, std::memory_order_release);

    const auto& cfg = agent.session_config();

    while (true) {
        char* line = readline("> ");
        if (line == nullptr) {
            std::cout << "\n";
            return 0;
        }

        std::string prompt(line);
        free(line);

        if (prompt.empty()) continue;

        if (prompt == "/exit" || prompt == "/quit") {
            return 0;
        }

        if (prompt == "/clear") {
            std::cout << "Note: /clear is not supported with AgentSession (history is managed internally).\n";
            continue;
        }

        if (prompt == "/stats" || prompt == "/session") {
            const int total  = agent.total_context_tokens();
            const int budget = cfg.context_size - cfg.compaction_reserve_tokens;
            const int pct    = budget > 0
                ? static_cast<int>((static_cast<double>(total) / budget) * 100.0)
                : 0;

            const std::string session_file = agent.session_path();
            std::cout << "\n=== Session Stats ===\n"
                      << "Session ID:       " << agent.session_id() << "\n"
                      << "Session file:     "
                      << (session_file.empty() ? "(not persisted)" : session_file) << "\n"
                      << "Messages:         " << agent.message_count() << "\n"
                      << "Tokens:           " << total << " / " << budget
                      << " (" << pct << "%)\n"
                      << "Compaction in ~  " << std::max(0, budget - total) << " tokens\n"
                      << "Compactions:      " << agent.compaction_count() << "\n";
            if (agent.compaction_count() > 0) {
                const auto& s = agent.last_compaction_stats();
                std::cout << "Last compaction:  " << s.tokens_before
                          << " -> " << s.tokens_after << " tokens\n";
            }
            std::cout << "Context size:     " << cfg.context_size << "\n"
                      << "Reserve tokens:   " << cfg.compaction_reserve_tokens << "\n"
                      << "Keep recent:      " << cfg.compaction_keep_recent_tokens << "\n"
                      << "Model:            " << agent.model() << "\n"
                      << "Thinking level:   " << thinking_level_to_string(agent.thinking_level()) << "\n"
                      << "\nCommands: /compact, /stats, /tokens, /thinking, /new, /branch, /exit\n"
                      << "=====================\n\n";
            continue;
        }

        if (prompt == "/tokens") {
            const int total  = agent.total_context_tokens();
            const int budget = cfg.context_size - cfg.compaction_reserve_tokens;
            std::cout << "\nTokens: " << total << " / " << budget << "\n"
                      << "=====================\n\n";
            continue;
        }

        if (prompt == "/compact") {
            std::cout << "\n[COMPACT] manually triggering compaction...\n";
            if (agent.compact()) {
                const auto& s = agent.last_compaction_stats();
                std::cout << "[COMPACT] " << s.tokens_before << " -> " << s.tokens_after << " tokens\n";
            } else {
                std::cout << "[COMPACT] no compaction needed (history already within budget)\n";
            }
            std::cout << format_token_budget(agent) << "\n";
            continue;
        }

        if (prompt == "/thinking") {
            agent.cycle_thinking_level();
            std::cout << "Thinking level: " << thinking_level_to_string(agent.thinking_level()) << "\n";
            continue;
        }

        if (prompt == "/new") {
            std::string newSessionId = agent.createNewSession();
            if (newSessionId.empty()) {
                std::cout << "[NEW] failed to create new session\n";
            }
            continue;
        }

        if (prompt == "/branch" || prompt.starts_with("/branch ")) {
            std::string args;
            if (prompt == "/branch") {
                args.clear();
            } else {
                args = prompt.substr(8);
            }
            size_t firstNonSpace = args.find_first_not_of(" \t");
            if (firstNonSpace != std::string::npos) {
                args = args.substr(firstNonSpace);
            } else {
                args.clear();
            }

            if (args.empty()) {
                agent.branch();
                continue;
            }

            if (args == "summary" || args.starts_with("summary ")) {
                std::string summaryArgs =
                    (args == "summary") ? "" : args.substr(8);
                firstNonSpace = summaryArgs.find_first_not_of(" \t");
                if (firstNonSpace != std::string::npos) {
                    summaryArgs = summaryArgs.substr(firstNonSpace);
                } else {
                    summaryArgs.clear();
                }

                std::optional<std::string> branchFromId;
                std::string summaryText;
                if (!summaryArgs.empty()) {
                    if (summaryArgs.size() == 8 &&
                        std::all_of(summaryArgs.begin(), summaryArgs.end(),
                                    [](unsigned char c) { return std::isxdigit(c) != 0; })) {
                        branchFromId = summaryArgs;
                        summaryText  = "Branch from entry " + summaryArgs;
                    } else {
                        summaryText = summaryArgs;
                    }
                } else {
                    summaryText = "Branch";
                }

                agent.branchWithSummary(summaryText, branchFromId);
                continue;
            }

            if (args.starts_with("from:")) {
                std::string entryId = args.substr(5);
                firstNonSpace = entryId.find_first_not_of(" \t");
                if (firstNonSpace != std::string::npos) {
                    entryId = entryId.substr(firstNonSpace);
                }
                agent.branchFrom(entryId);
                continue;
            }

            std::cerr << "Usage: /branch | /branch summary [text|entry-id] | /branch from:<entry-id>\n";
            continue;
        }

        // Reset cancel flag for each new user turn.
        global_cancel_flag.store(false, std::memory_order_release);

        TuiAnimation animation;

        // Drive between-tool-round animation via event handler.
        agent.set_event_handler([&animation](const AgentEvent& ev) {
            if (ev.type == AgentEvent::Type::ModelCallStart) {
                animation.resume_for_next_model_turn();
            }
        });

        animation.start(AnimationState::Thinking, "");

        add_history(prompt.c_str());

        const bool ok = agent.run(
            prompt,
            [&animation](const std::string& chunk) {
                animation.on_first_stream_chunk();
                animation.update(AnimationState::Generating, "");
                std::cout << chunk << std::flush;
            },
            &global_cancel_flag
        );

        animation.stop();

        if (near_compaction_threshold(agent)) {
            const int budget = cfg.context_size - cfg.compaction_reserve_tokens;
            std::cout << "\n[WARN] compaction in ~"
                      << (budget - agent.total_context_tokens()) << " tokens\n";
        }

        if (!ok) {
            // Distinguish interrupt from error: run() returns false on both.
            // The cancel flag tells us it was an interrupt.
            if (global_cancel_flag.load()) {
                std::cout << "\n[interrupted]\n";
            } else {
                std::cerr << "\nError: agent run failed\n";
            }
        } else {
            std::cout << "\n[done]\n";
        }

        if (interactive_debug) {
            print_interactive_turn_debug(agent);
        }

        std::cout << format_token_budget(agent) << "\n";
    }
}

}  // namespace coding_agent

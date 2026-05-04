#include "modes/interactive_mode.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <unistd.h>

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

std::optional<std::string> detect_build_dir(const char* argv0) {
    std::filesystem::path exe_path;
    char buf[4096];
    ssize_t len = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (len > 0) {
        buf[len] = '\0';
        exe_path = std::filesystem::path(buf);
    } else {
        exe_path = std::filesystem::path(argv0);
    }

    // exe_path should be like .../ports/coding-agent/build/coding-agent
    // build dir = exe_path.parent_path()
    // Check if CMakeCache.txt exists there
    std::filesystem::path build_dir = exe_path.parent_path();
    if (std::filesystem::exists(build_dir / "CMakeCache.txt")) {
        return build_dir.string();
    }

    // Fallback: check cwd/build/
    build_dir = std::filesystem::current_path() / "build";
    if (std::filesystem::exists(build_dir / "CMakeCache.txt")) {
        return build_dir.string();
    }

    return std::nullopt;
}

int run_build_command(const std::string& build_dir) {
    std::string cmd = "cmake --build \"" + build_dir + "\" --target coding-agent 2>&1";
    return std::system(cmd.c_str());
}

bool confirm_destructive_bash_on_tty(const std::string& command) {
    std::cerr << "Destructive bash command requested:\n"
              << command << "\nAllow execution? [y/N]: " << std::flush;
    std::ifstream tty("/dev/tty");
    if (!tty) {
        std::cerr << "(cannot open /dev/tty for confirmation)\n";
        return false;
    }
    std::string answer;
    std::getline(tty, answer);
    std::string normalized = answer;
    const auto first = normalized.find_first_not_of(" \t\r\n");
    if (first != std::string::npos) {
        normalized.erase(0, first);
    }
    const auto last = normalized.find_last_not_of(" \t\r\n");
    if (last != std::string::npos) {
        normalized.erase(last + 1);
    }
    for (char& ch : normalized) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return normalized == "y" || normalized == "yes";
}

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

const char* queue_mode_name(QueueMode mode) {
    return mode == QueueMode::All ? "all" : "one-at-a-time";
}

void print_queue_state(const AgentSession& agent) {
    const auto& steering = agent.steering_messages();
    const auto& follow_up = agent.follow_up_messages();
    std::cout << "\n=== Queue State ===\n";
    std::cout << "Steering mode:  " << queue_mode_name(agent.steering_mode()) << "\n";
    std::cout << "Follow-up mode: " << queue_mode_name(agent.follow_up_mode()) << "\n";
    std::cout << "Steering queue (" << steering.size() << "):\n";
    if (steering.empty()) {
        std::cout << "  (empty)\n";
    } else {
        for (size_t i = 0; i < steering.size(); ++i) {
            std::cout << "  " << (i + 1) << ". " << steering[i] << "\n";
        }
    }
    std::cout << "Follow-up queue (" << follow_up.size() << "):\n";
    if (follow_up.empty()) {
        std::cout << "  (empty)\n";
    } else {
        for (size_t i = 0; i < follow_up.size(); ++i) {
            std::cout << "  " << (i + 1) << ". " << follow_up[i] << "\n";
        }
    }
    std::cout << "===================\n\n";
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
    if (d.empty_completion_nudges > 0) {
        std::cout << "empty_completion_nudges: " << d.empty_completion_nudges << "\n";
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

int run_interactive_mode(AgentSession& agent, bool interactive_debug,
                         int argc, char** argv) {
    (void)argc;  // argc is preserved for execvp restart
    struct sigaction sa{};
    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, nullptr);

    global_cancel_flag.store(false, std::memory_order_release);

    agent.set_destructive_bash_confirm(confirm_destructive_bash_on_tty);

    const auto& cfg = agent.session_config();

    // Detect build directory once at startup
    static const auto build_dir = detect_build_dir(argv[0]);
    if (build_dir.has_value()) {
        std::cout << "[rebuild] Build directory detected: " << build_dir.value() << "\n";
    }

    bool rebuild_pending = false;

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

        if (prompt == "/help") {
            std::cout << "\n=== Commands ===\n"
                      << "  /help              Show this help message\n"
                      << "  /stats             Show session stats and token usage\n"
                      << "  /tokens            Show token usage\n"
                      << "  /compact           Manually trigger compaction\n"
                      << "  /thinking          Cycle thinking level\n"
                      << "  /queues            Show steering and follow-up queue state\n"
                      << "  /clear-queues      Clear all pending queues\n"
                      << "  /new               Create a new session\n"
                      << "  /branch            Branch current session\n"
                      << "  /branch summary [text|id]  Branch with summary\n"
                      << "  /branch from:<id>  Branch from specific entry\n"
                      << "  /exit, /quit       Exit the agent\n"
                      << "==================\n\n";
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
                      << "\nCommands: /compact, /stats, /tokens, /thinking, /queues, /clear-queues, /new, /branch, /exit\n"
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

        if (prompt == "/queues") {
            print_queue_state(agent);
            continue;
        }

        if (prompt == "/clear-queues") {
            agent.clear_all_queues();
            std::cout << "[queues] cleared\n";
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

        // --- /rebuild command ---
        if (prompt.starts_with("/rebuild")) {
            if (!build_dir.has_value()) {
                std::cerr << "[rebuild] Error: build directory not found. "
                          << "Please run 'cmake .. && cmake --build .' from the build directory.\n";
                continue;
            }

            std::string args = prompt.substr(8);
            size_t firstNonSpace = args.find_first_not_of(" \t");
            if (firstNonSpace != std::string::npos) {
                args = args.substr(firstNonSpace);
            } else {
                args.clear();
            }

            std::string normalized = args;
            for (char& ch : normalized) {
                ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
            }

            if (normalized == "confirm" || normalized == "yes" || normalized == "y") {
                std::cout << "[rebuild] Building...\n" << std::flush;

                // Reset SIGINT handler during build so Ctrl-C kills the build
                struct sigaction sa_build{};
                sa_build.sa_handler = SIG_DFL;
                sigemptyset(&sa_build.sa_mask);
                sigaction(SIGINT, &sa_build, nullptr);

                int result = run_build_command(build_dir.value());

                // Restore signal handler
                sigaction(SIGINT, &sa, nullptr);

                if (result != 0) {
                    std::cerr << "[rebuild] Build failed (exit code " << result << ")\n";
                    continue;
                }

                std::cout << "[rebuild] Build succeeded. Restarting...\n" << std::flush;

                // execvp replaces the current process
                execvp(argv[0], argv);

                // If execvp fails
                std::cerr << "[rebuild] execvp failed: " << strerror(errno) << "\n";
                continue;
            }

            if (rebuild_pending) {
                // Second confirmation - proceed with build
                std::cout << "[rebuild] Confirming rebuild...\n" << std::flush;
                rebuild_pending = false;
                std::cout << "[rebuild] Building...\n" << std::flush;

                struct sigaction sa_build{};
                sa_build.sa_handler = SIG_DFL;
                sigemptyset(&sa_build.sa_mask);
                sigaction(SIGINT, &sa_build, nullptr);

                int result = run_build_command(build_dir.value());

                sigaction(SIGINT, &sa, nullptr);

                if (result != 0) {
                    std::cerr << "[rebuild] Build failed (exit code " << result << ")\n";
                    continue;
                }

                std::cout << "[rebuild] Build succeeded. Restarting...\n" << std::flush;
                execvp(argv[0], argv);
                std::cerr << "[rebuild] execvp failed: " << strerror(errno) << "\n";
                continue;
            }

            // First invocation - set pending and warn
            rebuild_pending = true;
            std::cout << "[rebuild] WARNING: Rebuilding will restart the agent. "
                      << "Current session will be preserved.\n"
                      << "[rebuild] Pending rebuild. Type '/rebuild confirm' to proceed.\n";
            continue;
        }

        // --- end /rebuild ---

        // Reset cancel flag for each new user turn.
        global_cancel_flag.store(false, std::memory_order_release);

        TuiAnimation animation;

        // Drive between-tool-round animation via event handler.
        agent.set_event_handler([&animation](const AgentEvent& ev) {
            if (ev.type == AgentEvent::Type::ModelCallStart) {
                animation.resume_for_next_model_turn();
                return;
            }
            if (ev.type == AgentEvent::Type::QueueUpdate) {
                std::cout << "\n[queue] steering=" << ev.steering_messages.size()
                          << ", follow-up=" << ev.follow_up_messages.size() << "\n";
                return;
            }
            if (ev.type == AgentEvent::Type::AutoRetryStart) {
                std::cout << "\n[retry #" << ev.retry_attempt << "/" << ev.retry_max_attempts
                          << "] Waiting " << ev.retry_delay_ms << "ms...\n";
                return;
            }
            if (ev.type == AgentEvent::Type::AutoRetryEnd) {
                if (ev.retry_success) {
                    std::cout << "\n[retry] Success after " << ev.retry_attempt << " attempts\n";
                } else {
                    std::cout << "\n[retry] Failed after " << ev.retry_attempt << " attempts";
                    if (!ev.retry_final_error.empty()) {
                        std::cout << ": " << ev.retry_final_error;
                    }
                    std::cout << "\n";
                }
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

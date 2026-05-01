#include "agent.hpp"

#include <filesystem>
#include <iostream>
#include <string>

#include <unistd.h>

#include "agent_session.hpp"
#include "branch_summary.hpp"
#include "config.hpp"
#include "modes/interactive_mode.hpp"
#include "modes/print_mode.hpp"
#include "providers/llama_cpp_provider.hpp"
#include "session.hpp"
#include "tools/tool_registry.hpp"

namespace coding_agent {
namespace {

bool should_run_print_mode(const Config& config) {
    if (config.print_mode || config.prompt.has_value()) return true;
    return !isatty(STDIN_FILENO);
}

AgentSessionConfig make_session_config(const Config& c) {
    AgentSessionConfig cfg{};
    cfg.provider                   = c.provider;
    cfg.base_url                   = c.base_url;
    cfg.model                      = c.model;
    cfg.api_key                    = c.api_key;
    cfg.cwd                        = c.cwd;
    cfg.system_prompt_path         = c.system_prompt_path;
    cfg.append_system_prompts      = c.append_system_prompts;
    cfg.session_id                 = c.session_id;
    cfg.max_tokens                 = c.max_tokens;
    cfg.context_size               = c.context_size;
    cfg.compaction_reserve_tokens  = c.compaction_reserve_tokens;
    cfg.compaction_keep_recent_tokens = c.compaction_keep_recent_tokens;
    cfg.max_tool_iterations        = c.max_tool_iterations;
    cfg.temperature                = c.temperature;
    cfg.stream                     = c.stream;
    cfg.no_tools                   = c.no_tools;
    cfg.no_context_files           = c.no_context_files;
    cfg.compaction_fail_fast       = c.compaction_fail_fast;
    cfg.auto_compaction            = true;
    cfg.initial_active_tools       = c.initial_active_tools;
    return cfg;
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

    if (!std::filesystem::exists(config->cwd) || !std::filesystem::is_directory(config->cwd)) {
        std::cerr << "Error: cwd is not a valid directory: " << config->cwd << "\n";
        return 1;
    }

    std::filesystem::current_path(config->cwd);

    LlamaCppProvider provider(config->base_url, config->api_key);
    ToolRegistry tools;
    if (!config->no_tools) {
        register_builtin_tools(tools);
    }

    SessionStore session(config->cwd);

    // Branch summary: detect previous session and generate summary if needed.
    std::optional<std::filesystem::path> handoff_old_file;
    std::optional<std::string> handoff_old_leaf;
    if (config->branch_summary) {
        if (config->new_session) {
            if (const auto prev = latest_session_path_in_dir(session.session_dir()); prev.has_value()) {
                SessionGraph graph;
                std::string graph_error;
                if (load_session_graph(prev->string(), graph, graph_error) && !graph.leaf_id.empty()) {
                    handoff_old_file = prev;
                    handoff_old_leaf = graph.leaf_id;
                }
            }
        } else if (config->session_id.has_value()) {
            if (const auto latest = latest_session_path_in_dir(session.session_dir()); latest.has_value()) {
                if (latest->stem().string() != config->session_id.value()) {
                    SessionGraph graph;
                    std::string graph_error;
                    if (load_session_graph(latest->string(), graph, graph_error) && !graph.leaf_id.empty()) {
                        handoff_old_file = latest;
                        handoff_old_leaf = graph.leaf_id;
                    }
                }
            }
        }
    }

    session.start_or_resume(config->session_id, config->new_session);

    if (config->branch_summary && handoff_old_file.has_value() && handoff_old_leaf.has_value()) {
        SessionGraph old_graph;
        std::string graph_error;
        if (load_session_graph(handoff_old_file->string(), old_graph, graph_error)) {
            const std::vector<SessionNode> collected =
                collect_entries_for_branch_summary(old_graph, handoff_old_leaf.value(), "");
            std::string gen_error;
            const BranchSummaryResult branch_result = generate_branch_summary(
                collected,
                provider,
                config->model,
                config->context_size,
                config->compaction_reserve_tokens,
                gen_error
            );
            BranchSummaryEvent branch_event{
                .summary              = branch_result.summary,
                .source_session_id    = handoff_old_file->stem().string(),
                .read_files           = branch_result.read_files,
                .modified_files       = branch_result.modified_files,
                .handoff_source_leaf_id = handoff_old_leaf.value(),
            };
            std::string append_error;
            (void)session.append_branch_summary(branch_event, append_error);
        }
    }

    AgentSession agent(make_session_config(config.value()), provider, tools, session);

    if (should_run_print_mode(config.value())) {
        return run_print_mode(agent, config->prompt.value_or(""));
    }
    return run_interactive_mode(agent);
}

}  // namespace coding_agent

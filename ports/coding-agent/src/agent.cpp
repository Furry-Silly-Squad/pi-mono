#include "agent.hpp"

#include <filesystem>
#include <iostream>
#include <memory>
#include <string>

#include <unistd.h>

#include <nlohmann/json.hpp>

#include "agent_session.hpp"
#include "branch_summary.hpp"
#include "config.hpp"
#include "modes/interactive_mode.hpp"
#include "modes/print_mode.hpp"
#include "providers/llama_cpp_provider.hpp"
#include "session_entry.hpp"
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
    cfg.interactive_debug          = c.interactive_debug;
    cfg.max_empty_completion_nudges = c.max_empty_completion_nudges;
    cfg.retry_enabled              = c.retry_enabled;
    cfg.retry_max_retries          = c.retry_max_retries;
    cfg.retry_base_delay_ms        = c.retry_base_delay_ms;
    cfg.retry_max_retry_delay_ms   = c.retry_max_retry_delay_ms;
    cfg.retry_timeout_ms           = c.retry_timeout_ms;
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

    const std::string workspace_sessions = session_directory_for_cwd(config->cwd);

    // Branch summary: detect previous session and generate summary if needed (single open of source session).
    std::optional<std::filesystem::path> handoff_old_file;
    std::optional<std::string> handoff_old_leaf;
    std::unique_ptr<SessionManager> handoff_old_mgr;
    if (config->branch_summary) {
        if (config->new_session) {
            if (const auto prev = latest_session_path_in_dir(workspace_sessions); prev.has_value()) {
                auto mgr = SessionManager::open(prev->string(), "", config->cwd);
                if (mgr && mgr->getLeafId().has_value()) {
                    handoff_old_file = prev;
                    handoff_old_leaf = mgr->getLeafId().value();
                    handoff_old_mgr    = std::move(mgr);
                }
            }
        } else if (config->session_id.has_value()) {
            if (const auto latest = latest_session_path_in_dir(workspace_sessions); latest.has_value()) {
                if (latest->stem().string() != config->session_id.value()) {
                    auto mgr = SessionManager::open(latest->string(), "", config->cwd);
                    if (mgr && mgr->getLeafId().has_value()) {
                        handoff_old_file = latest;
                        handoff_old_leaf = mgr->getLeafId().value();
                        handoff_old_mgr    = std::move(mgr);
                    }
                }
            }
        }
    }

    std::unique_ptr<SessionManager> session_mgr;
    if (config->new_session) {
        session_mgr = SessionManager::create(config->cwd, "");
    } else if (config->session_id.has_value()) {
        session_mgr =
            SessionManager::openBySessionId(config->cwd, config->session_id.value(), "");
        if (!session_mgr) {
            std::cerr << "Error: session not found: " << *config->session_id << "\n";
            return 1;
        }
    } else {
        session_mgr = SessionManager::continueRecent(config->cwd, "");
    }

    if (config->branch_summary && handoff_old_mgr && handoff_old_leaf.has_value()) {
        const std::vector<SessionEntry> collected =
            collect_entries_for_branch_summary(*handoff_old_mgr, handoff_old_leaf.value(), "");
        std::string gen_error;
        std::cerr << "Summarizing previous session...\n" << std::flush;
        const BranchSummaryResult branch_result = generate_branch_summary(
            collected,
            provider,
            config->model,
            config->context_size,
            config->compaction_reserve_tokens,
            gen_error
        );
        nlohmann::json branch_details;
        branch_details["source_session_id"]    = handoff_old_file->stem().string();
        branch_details["read_files"]           = branch_result.read_files;
        branch_details["modified_files"]       = branch_result.modified_files;
        branch_details["handoff_source_leaf_id"] = handoff_old_leaf.value();
        session_mgr->appendBranchSummary(handoff_old_leaf.value(), branch_result.summary,
                                         std::make_optional(branch_details));
    }

    AgentSession agent(make_session_config(config.value()), provider, tools, std::move(session_mgr));

    if (should_run_print_mode(config.value())) {
        return run_print_mode(agent, config->prompt.value_or(""));
    }
    return run_interactive_mode(agent, config->interactive_debug);
}

}  // namespace coding_agent

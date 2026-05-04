#pragma once

#include <optional>
#include <string>
#include <vector>

namespace coding_agent {

/// Configuration for a single LLM server.
struct ServerConfig {
    std::string id;             // Unique identifier (e.g., "laptop-gpu")
    std::string name;           // Human-readable name (e.g., "MacBook M3 GPU")
    std::string baseUrl;        // Server endpoint (e.g., "http://127.0.0.1:8080")
    std::string apiKey;         // API key (empty if not required)
    std::string modelId;        // Default model for this server
    int contextLength = 8192;   // Max context window
    int maxOutputTokens = 4096; // Max output tokens per call
    std::string description;    // Free-text description for LLM routing decisions
    std::vector<std::string> capabilities;
    std::vector<std::string> preferredFor;
    bool gpu = true;            // Whether this server has GPU acceleration
    std::vector<std::string> contextFiles;  // Files to pass to the sub-agent
};

/// Global server configuration loaded from ~/.pi/servers.json.
class ServerConfigStore {
public:
    /// Load server configs from the global config file.
    /// Returns nullopt on failure (file not found, parse error, etc.).
    static std::optional<ServerConfigStore> load(const std::string& homeDir = "");

    /// Get all configured servers.
    const std::vector<ServerConfig>& getServers() const { return servers_; }

    /// Find a server by ID. Returns nullopt if not found.
    std::optional<ServerConfig> findServer(const std::string& id) const;

    /// Find a server by URL. Returns nullopt if not found.
    std::optional<ServerConfig> findServerByUrl(const std::string& url) const;

    /// Get a formatted list of servers for injection into the decomposition prompt.
    std::string getPromptDescription() const;

private:
    ServerConfigStore(std::vector<ServerConfig> servers) : servers_(std::move(servers)) {}

    std::vector<ServerConfig> servers_;
};

}  // namespace coding_agent

#include "server_config.hpp"

#include <fstream>
#include <iostream>
#include <sstream>
#include <unordered_map>

#include <nlohmann/json.hpp>

namespace coding_agent {

namespace fs = std::filesystem;

std::optional<ServerConfigStore> ServerConfigStore::load(const std::string& homeDir) {
    const char* home = std::getenv("HOME");
    std::string baseDir = homeDir.empty() ? (home ? home : ".") : homeDir;
    fs::path configPath = fs::path(baseDir) / ".pi" / "servers.json";

    if (!fs::exists(configPath)) {
        return std::nullopt;
    }

    std::ifstream file(configPath);
    if (!file.is_open()) {
        std::cerr << "Warning: could not open server config file: " << configPath << "\n";
        return std::nullopt;
    }

    std::stringstream buffer;
    buffer << file.rdbuf();
    std::string contents = buffer.str();

    if (contents.empty()) {
        return std::nullopt;
    }

    nlohmann::json j;
    try {
        j = nlohmann::json::parse(contents, nullptr, false);
        if (j.is_discarded()) {
            std::cerr << "Warning: server config file is not valid JSON: " << configPath << "\n";
            return std::nullopt;
        }
    } catch (const std::exception& ex) {
        std::cerr << "Warning: failed to parse server config: " << ex.what() << "\n";
        return std::nullopt;
    }

    std::vector<ServerConfig> servers;

    if (j.contains("servers") && j["servers"].is_array()) {
        for (const auto& entry : j["servers"]) {
            ServerConfig server;

            server.id = entry.value("id", "");
            server.name = entry.value("name", "");
            server.baseUrl = entry.value("baseUrl", "");
            server.apiKey = entry.value("apiKey", "");
            server.modelId = entry.value("modelId", "");
            server.contextLength = entry.value("contextLength", 8192);
            server.maxOutputTokens = entry.value("maxOutputTokens", 4096);
            server.description = entry.value("description", "");
            server.gpu = entry.value("gpu", true);

            if (entry.contains("capabilities") && entry["capabilities"].is_array()) {
                for (const auto& cap : entry["capabilities"]) {
                    server.capabilities.push_back(cap.get<std::string>());
                }
            }

            if (entry.contains("preferredFor") && entry["preferredFor"].is_array()) {
                for (const auto& pf : entry["preferredFor"]) {
                    server.preferredFor.push_back(pf.get<std::string>());
                }
            }

            if (!server.id.empty() && !server.baseUrl.empty()) {
                servers.push_back(std::move(server));
            }
        }
    }

    if (servers.empty()) {
        return std::nullopt;
    }

    return ServerConfigStore(std::move(servers));
}

std::optional<ServerConfig> ServerConfigStore::findServer(const std::string& id) const {
    for (const auto& server : servers_) {
        if (server.id == id) {
            return server;
        }
    }
    return std::nullopt;
}

std::optional<ServerConfig> ServerConfigStore::findServerByUrl(const std::string& url) const {
    for (const auto& server : servers_) {
        if (server.baseUrl == url) {
            return server;
        }
    }
    return std::nullopt;
}

std::string ServerConfigStore::getPromptDescription() const {
    if (servers_.empty()) {
        return "";
    }

    std::ostringstream oss;
    oss << "Available servers:\n";
    for (const auto& server : servers_) {
        oss << "- " << server.id << " (" << server.name << "): ";
        if (!server.description.empty()) {
            oss << server.description;
        } else {
            oss << server.baseUrl;
            if (server.gpu) oss << " (GPU)";
            oss << " context=" << server.contextLength;
        }
        if (!server.preferredFor.empty()) {
            oss << ". Preferred for: ";
            for (size_t i = 0; i < server.preferredFor.size(); ++i) {
                if (i > 0) oss << ", ";
                oss << server.preferredFor[i];
            }
        }
        oss << "\n";
    }
    return oss.str();
}

}  // namespace coding_agent

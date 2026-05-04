#include "server_config.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace {

int fail(const char* msg) {
    std::cerr << "FAIL: " << msg << "\n";
    return 1;
}

int pass() {
    return 0;
}

}  // namespace

int main() {
    using namespace coding_agent;
    namespace fs = std::filesystem;

    // Create a temporary directory for the test config
    fs::path testDir = fs::temp_directory_path() / ("pi-server-config-test-" + std::to_string(std::time(nullptr)));
    fs::create_directories(testDir / ".pi");
    fs::path configFile = testDir / ".pi" / "servers.json";

    // Write a test config file
    std::ofstream out(configFile);
    out << R"(
{
    "servers": [
        {
            "id": "laptop-gpu",
            "name": "MacBook M3 GPU",
            "baseUrl": "http://127.0.0.1:8080",
            "apiKey": "",
            "modelId": "llama3.1-8b",
            "contextLength": 8192,
            "maxOutputTokens": 4096,
            "description": "Local MacBook GPU. Fast for small tasks.",
            "capabilities": ["fast", "local", "small-model"],
            "preferredFor": ["quick-prototypes", "code-completion"],
            "gpu": true
        },
        {
            "id": "dev-server",
            "name": "Dev Server A100",
            "baseUrl": "https://ai.internal.example.com:8080",
            "apiKey": "sk-test-key-123",
            "modelId": "qwen2.5-72b",
            "contextLength": 131072,
            "maxOutputTokens": 32768,
            "description": "Remote server with large context window.",
            "capabilities": ["large-context", "strong-reasoning"],
            "preferredFor": ["complex-refactors", "documentation"],
            "gpu": true
        }
    ]
}
)";
    out.close();

    // Test 1: Load server config
    {
        auto store = ServerConfigStore::load(testDir.string());
        if (!store.has_value()) {
            return fail("load() should succeed with valid config");
        }
        if (store->getServers().size() != 2) {
            return fail(("expected 2 servers, got " + std::to_string(store->getServers().size())).c_str());
        }
    }

    // Test 2: Find server by ID
    {
        auto store = ServerConfigStore::load(testDir.string());
        if (!store.has_value()) {
            return fail("load() should succeed");
        }

        auto laptop = store->findServer("laptop-gpu");
        if (!laptop.has_value()) {
            return fail("should find laptop-gpu server");
        }
        if (laptop->modelId != "llama3.1-8b") {
            return fail("expected modelId llama3.1-8b");
        }
        if (laptop->contextLength != 8192) {
            return fail("expected contextLength 8192");
        }
        if (laptop->capabilities.size() != 3) {
            return fail("expected 3 capabilities");
        }
    }

    // Test 3: Find server by URL
    {
        auto store = ServerConfigStore::load(testDir.string());
        if (!store.has_value()) {
            return fail("load() should succeed");
        }

        auto dev = store->findServerByUrl("https://ai.internal.example.com:8080");
        if (!dev.has_value()) {
            return fail("should find dev-server by URL");
        }
        if (dev->apiKey != "sk-test-key-123") {
            return fail("expected apiKey sk-test-key-123");
        }
    }

    // Test 4: Not found returns nullopt
    {
        auto store = ServerConfigStore::load(testDir.string());
        if (!store.has_value()) {
            return fail("load() should succeed");
        }

        if (store->findServer("nonexistent").has_value()) {
            return fail("should return nullopt for unknown server");
        }
        if (store->findServerByUrl("http://nonexistent:9999").has_value()) {
            return fail("should return nullopt for unknown URL");
        }
    }

    // Test 5: Prompt description is non-empty
    {
        auto store = ServerConfigStore::load(testDir.string());
        if (!store.has_value()) {
            return fail("load() should succeed");
        }

        std::string desc = store->getPromptDescription();
        if (desc.empty()) {
            return fail("prompt description should not be empty");
        }
        if (desc.find("laptop-gpu") == std::string::npos) {
            return fail("prompt description should include server IDs");
        }
        if (desc.find("MacBook M3 GPU") == std::string::npos) {
            return fail("prompt description should include server names");
        }
    }

    // Test 6: Missing file returns nullopt
    {
        auto store = ServerConfigStore::load(testDir.string() + "/nonexistent");
        if (store.has_value()) {
            return fail("should return nullopt when config file does not exist");
        }
    }

    // Test 7: Invalid JSON returns nullopt
    {
        fs::path badConfig = testDir / ".pi" / "bad-servers.json";
        std::ofstream badOut(badConfig);
        badOut << "{ this is not valid json }";
        badOut.close();

        // Temporarily replace servers.json with invalid JSON
        fs::path backup = testDir / ".pi" / "servers.json.bak";
        fs::rename(configFile, backup);
        fs::rename(badConfig, configFile);

        auto store = ServerConfigStore::load(testDir.string());
        if (store.has_value()) {
            return fail("should return nullopt for invalid JSON");
        }

        // Restore original
        fs::rename(backup, configFile);
    }

    // Test 8: Empty servers array returns nullopt
    {
        fs::path emptyConfig = testDir / ".pi" / "empty-servers.json";
        std::ofstream emptyOut(emptyConfig);
        emptyOut << R"({"servers": []})";
        emptyOut.close();

        // Temporarily replace servers.json with empty array
        fs::path backup = testDir / ".pi" / "servers.json.bak";
        fs::rename(configFile, backup);
        fs::rename(emptyConfig, configFile);

        auto store = ServerConfigStore::load(testDir.string());
        if (store.has_value()) {
            return fail("should return nullopt for empty servers array");
        }

        // Restore original
        fs::rename(backup, configFile);
    }

    // Test 9: Server with missing required fields (id or baseUrl) is skipped
    {
        fs::path partialConfig = testDir / ".pi" / "partial-servers.json";
        std::ofstream partialOut(partialConfig);
        partialOut << R"({
            "servers": [
                {
                    "id": "partial",
                    "baseUrl": "http://localhost:9999"
                },
                {
                    "id": "valid-server",
                    "name": "Valid",
                    "baseUrl": "http://valid.example.com:8080",
                    "modelId": "test-model",
                    "contextLength": 4096,
                    "maxOutputTokens": 2048,
                    "gpu": true
                },
                {
                    "name": "MissingId",
                    "baseUrl": "http://noid.example.com:8080",
                    "modelId": "test",
                    "contextLength": 4096,
                    "maxOutputTokens": 2048,
                    "gpu": true
                },
                {
                    "id": "MissingUrl",
                    "name": "NoUrl",
                    "modelId": "test",
                    "contextLength": 4096,
                    "maxOutputTokens": 2048,
                    "gpu": true
                }
            ]
        })";
        partialOut.close();

        // Temporarily replace servers.json
        fs::path backup = testDir / ".pi" / "servers.json.bak";
        fs::rename(configFile, backup);
        fs::rename(partialConfig, configFile);

        auto store = ServerConfigStore::load(testDir.string());
        if (!store.has_value()) {
            return fail("should succeed with partial servers config");
        }
        if (store->getServers().size() != 2) {
            return fail(("expected 2 valid servers (partial + valid-server), got " + std::to_string(store->getServers().size())).c_str());
        }

        // Restore original
        fs::rename(backup, configFile);
    }

    // Test 10: Default values for optional fields
    {
        fs::path minimalConfig = testDir / ".pi" / "minimal-servers.json";
        std::ofstream minimalOut(minimalConfig);
        minimalOut << R"({
            "servers": [
                {
                    "id": "minimal",
                    "baseUrl": "http://minimal.example.com:8080"
                }
            ]
        })";
        minimalOut.close();

        // Temporarily replace servers.json
        fs::path backup = testDir / ".pi" / "servers.json.bak";
        fs::rename(configFile, backup);
        fs::rename(minimalConfig, configFile);

        auto store = ServerConfigStore::load(testDir.string());
        if (!store.has_value()) {
            return fail("should succeed with minimal server config");
        }
        auto server = store->findServer("minimal");
        if (!server.has_value()) {
            return fail("should find minimal server");
        }
        if (server->contextLength != 8192) {
            return fail("expected default contextLength 8192");
        }
        if (server->maxOutputTokens != 4096) {
            return fail("expected default maxOutputTokens 4096");
        }
        if (server->gpu != true) {
            return fail("expected default gpu true");
        }
        if (!server->description.empty()) {
            return fail("expected empty description for minimal config");
        }

        // Restore original
        fs::rename(backup, configFile);
    }

    // Cleanup
    fs::remove_all(testDir);

    std::cout << "coding-agent-server-config-test: ok\n";
    return pass();
}

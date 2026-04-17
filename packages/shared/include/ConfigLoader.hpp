#pragma once

#include "Context.hpp"

#include <string>
#include <map>
#include <optional>
#include <mutex>
#include <vector>
#include <unordered_map>

namespace firmius::shared {

struct ModelOption {
    std::string providerId;
    std::string modelId;
    std::string variantName;
};

struct ModelRouteCategory {
    std::vector<ModelOption> models;
};


struct McpServerConfig {
    std::string transport = "stdio";

    // stdio transport fields
    std::string command;
    std::vector<std::string> args;
    std::map<std::string, std::string> env;
    std::string cwd;

    // http transport fields
    std::string url;
    std::string authHeader = "Authorization";
    std::string authBearerToken;
    bool allowInsecureTls = false;
    std::string caCertPath;

    bool enabled = true;
};
using McpStdioServerConfig = McpServerConfig;
struct UserConfig {
    using RollingMemoryConfig = firmius::shared::AgentConfig::RollingMemoryConfig;
    std::string defaultProviderId = "nanogpt";
    std::string defaultModelId = "zai-org/glm-4.6:thinking";
    std::string defaultModelVariant;                      // Selected model variant (e.g., "low", "medium", "max")
    std::string defaultLeadPersona = "lead";
    float defaultTemperature = 0.7f;
    std::optional<uint32_t> defaultMaxTokens;
    bool dangerouslySkipPermissions = false;
    std::map<std::string, std::string> apiKeys;           // providerId -> apiKey
    std::map<std::string, std::string> providerOptions;   // key -> value for provider-specific settings
    std::map<std::string, ModelRouteCategory> modelRouterCategories; // category -> provider/model/variant
    std::map<std::string, std::string> purposeRoutes; // purpose/persona -> category
    std::string defaultRouteCategory;
    bool enableSubagentRouteFallback = true;
    std::vector<std::string> subagentRouteFallbackOrder;
    bool showInternalNudges = false;
    bool hideErrors = false;                              // Hide errors in chat display
    RollingMemoryConfig rollingMemory;
    bool insanityDetectionEnabled = true;
    int insanityRepetitionThreshold = 3;
    std::uint64_t insanityMaxTokenThreshold = 50000;
    int maxInsanityRetries = 2;
    std::map<std::string, McpServerConfig> mcpServers; // serverName -> MCP server config
};

class ConfigLoader {
public:
    static ConfigLoader& instance();

    void load();
    void save() const;

    const UserConfig& getConfig() const;
    // Runtime preferred model tracking per category (not persisted)
    void setPreferredModelKey(const std::string& category, const std::string& key) const;
    std::string getPreferredModelKey(const std::string& category) const;
    void clearPreferredModelKey(const std::string& category) const;
    void updateConfig(const UserConfig& config);

    std::string getConfigPath() const;

private:
    ConfigLoader() = default;
    ConfigLoader(const ConfigLoader&) = delete;
    ConfigLoader& operator=(const ConfigLoader&) = delete;

    void loadImpl();

    mutable std::unordered_map<std::string, std::string> preferredModelKey_;
    UserConfig config_;
    mutable std::mutex mutex_;
    bool loaded_ = false;
};

} // namespace firmius::shared

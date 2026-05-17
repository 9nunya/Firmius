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

struct ProviderVariantConfig {
    std::string label;
    std::string requestJson = "{}";
    std::string description;
};

struct ProviderModelConfig {
    std::string defaultVariant;
    std::map<std::string, ProviderVariantConfig> variants;
    bool overrideContextWindow = false;
    std::uint32_t contextWindow = 0;
    bool overrideMaxOutputTokens = false;
    std::uint32_t maxOutputTokens = 0;
    bool overrideModalities = false;
    std::vector<std::string> modalities;
    bool overrideSupportsReasoning = false;
    bool supportsReasoning = false;
};

struct ProviderDefaultsConfig {
    float temperature = 0.7f;
    std::optional<uint32_t> maxTokens;
    bool streamUsage = true;
};

struct RetryPolicyConfig {
    int maxRetries = 5;
    int baseDelayMs = 1000;
    int maxDelayMs = 30000;
    double jitterMin = 0.5;
    double jitterMax = 1.0;
    bool useSharedBackoffSequence = true;
    bool respectRetryAfter = true;
    int timeoutSeconds = 300;
    int connectTimeoutSeconds = 10;
    std::vector<int> retryHttpStatuses{408, 429, 500, 501, 502, 503, 504, 529};
    std::vector<int> nonRetryHttpStatuses{401, 403, 404, 422};
    std::vector<std::string> retryCurlErrors{"timeout", "connect", "dns", "send", "recv"};
};

struct ProviderProfileConfig {
    std::string authMode = "api_key";
    std::string kind = "openai_compatible";
    std::string displayName;
    bool enabled = true;
    std::string baseUrl;
    std::string modelsEndpoint = "/models";
    std::string chatEndpoint = "/chat/completions";
    std::string messagesEndpoint = "/v1/messages";
    std::string apiKeyRef;
    std::string defaultApiKey;
    bool allowMissingApiKey = false;
    std::map<std::string, std::string> headers;
    ProviderDefaultsConfig defaults;
    std::string reasoningFieldName = "reasoning_effort";
    std::string anthropicVersion = "2023-06-01";
    std::string betaHeader;
    RetryPolicyConfig retry;
    std::map<std::string, ProviderModelConfig> modelVariants;
};

struct UserConfig {
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
    bool insanityDetectionEnabled = true;
    int insanityRepetitionThreshold = 3;
    std::uint64_t insanityMaxTokenThreshold = 50000;
    int maxInsanityRetries = 2;
    std::map<std::string, McpServerConfig> mcpServers; // serverName -> MCP server config
    RetryPolicyConfig providerRetryDefaults;
    std::map<std::string, RetryPolicyConfig> providerRetryPolicies;
    std::map<std::string, ProviderProfileConfig> providers;
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

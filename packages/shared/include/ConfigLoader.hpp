#pragma once

#include <string>
#include <map>
#include <optional>
#include <mutex>
#include <vector>

namespace firmius::shared {

struct ModelRouteCategory {
    std::string providerId;
    std::string modelId;
    std::string variantName;
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
};

class ConfigLoader {
public:
    static ConfigLoader& instance();

    void load();
    void save() const;

    const UserConfig& getConfig() const;
    void updateConfig(const UserConfig& config);

    std::string getConfigPath() const;

private:
    ConfigLoader() = default;
    ConfigLoader(const ConfigLoader&) = delete;
    ConfigLoader& operator=(const ConfigLoader&) = delete;

    void loadImpl();

    UserConfig config_;
    mutable std::mutex mutex_;
    bool loaded_ = false;
};

} // namespace firmius::shared

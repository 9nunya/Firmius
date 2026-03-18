#pragma once

#include <string>
#include <map>
#include <optional>
#include <mutex>

namespace firmius::shared {

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

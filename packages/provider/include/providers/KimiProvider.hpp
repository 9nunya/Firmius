#pragma once

#include "providers/BaseAnthropicProvider.hpp"
#include <string>
#include <vector>

namespace firmius::provider {

/**
 * @brief Kimi provider for Moonshot AI's Kimi API
 *
 * Anthropic-compatible provider for Moonshot AI's Kimi models.
 * Uses API key authentication with base URL: https://api.kimi.com/coding/v1
 *
 * Environment variables:
 * - KIMI_API_KEY: Primary API key
 * - KIMI_API_KEY_1, KIMI_API_KEY_2, ...: Additional keys for rotation
 *
 * Supports models like: kimi-k2.5, kimi-dev, etc.
 */
class KimiProvider : public BaseAnthropicProvider {
public:
    /**
     * @brief Construct a new Kimi Provider
     * @param initialKeys Optional initial API keys to use
     */
    explicit KimiProvider(const std::vector<std::string>& initialKeys = {});

    /**
     * @brief Get the provider ID
     * @return "kimi"
     */
    std::string getId() const override { return "kimi"; }

    /**
     * @brief Get custom headers for Kimi API requests
     * @return Map of header name to value
     */
    std::map<std::string, std::string> getHeaders() override;

    /**
     * @brief Get the base URL for Kimi API requests
     * @return "https://api.kimi.com/coding/v1"
     */
    std::string getBaseUrl() const override;

    /**
     * @brief List available models from Kimi API
     * @return Vector of ModelInfo
     */
    std::vector<firmius::shared::ModelInfo> listModels() override;
};

} // namespace firmius::provider

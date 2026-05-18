#ifndef FIRMIUS_PROVIDER_OPENROUTER_PROVIDER_HPP
#define FIRMIUS_PROVIDER_OPENROUTER_PROVIDER_HPP

#include "providers/BaseOpenAIProvider.hpp"
#include <cstdint>
#include <optional>
#include <string>

namespace firmius::provider {

/**
 * @brief Provider implementation for OpenRouter.
 */
class OpenRouterProvider : public BaseOpenAIProvider {
public:
    struct KeyQuotaInfo {
        std::optional<double> limit;
        std::optional<double> limitRemaining;
        std::string limitReset;
        double usage = 0.0;
        std::string label;
        bool isFreeTier = false;
    };

    /**
     * @brief Constructs an OpenRouterProvider.
     * @param apiKey The OpenRouter API key.
     */
    OpenRouterProvider(const std::string& apiKey);

    static std::optional<KeyQuotaInfo>
    parseKeyQuotaInfoResponse(const std::string& response);

protected:
    /**
     * @brief Returns headers required by OpenRouter (HTTP-Referer, X-Title).
     */
    std::map<std::string, std::string> buildHeadersForApiKey(const std::string& apiKey) override;

    /**
     * @brief Returns "reasoning" field as expected by OpenRouter.
     */
    std::string getReasoningFieldName() const override;

    /**
     * @brief Override to inject cache_control markers when routing to
     *        Anthropic models. OpenRouter passes Anthropic-style
     *        cache_control through to upstream Claude models for explicit
     *        prompt caching (90% discount on hits).
     */
    std::string prepareRequestBody(const firmius::shared::AgentHistory &history,
                                   const firmius::provider::ProviderOptions &opts) override;

    bool supportsQuotaTracking() const override { return true; }
    void refreshQuotas() override;
    std::map<std::string, std::vector<firmius::shared::QuotaBucket>>
    getAllQuotas() const override;
    std::optional<APIKeyAccount *>
    getAvailableAccount(
        const std::optional<std::string> &modelId = std::nullopt) override;

    virtual std::optional<KeyQuotaInfo>
    fetchKeyQuotaInfo(const APIKeyAccount& acc) const;

    RateLimitSwitchResult handleRateLimitAndMaybeSwitch(
        APIKeyAccount& currentAccount,
        const std::optional<std::string>& modelId,
        int headerDelayMs,
        int rateLimitAttempt,
        int64_t rateLimitResetMs = 0) override;

public:
    /**
     * @brief Fetches models from OpenRouter with dynamic pricing.
     */
    std::vector<firmius::shared::ModelInfo> listModels() override;
};

}

#endif

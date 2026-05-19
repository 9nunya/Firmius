#ifndef FIRMIUS_PROVIDER_NANOGPTPROVIDER_HPP
#define FIRMIUS_PROVIDER_NANOGPTPROVIDER_HPP

#include "providers/BaseOpenAIProvider.hpp"
#include <vector>
#include <string>

namespace firmius::provider {

/**
 * @brief Provider implementation for NanoGPT.
 * Supports API key rotation and detailed model discovery.
 */
class NanoGPTProvider : public BaseOpenAIProvider {
public:
    /**
     * @brief Constructs a NanoGPTProvider.
     * @param apiKeys Initial list of API keys. If empty, loads from env.
     */
    NanoGPTProvider(const std::vector<std::string>& apiKeys = {});

    /**
     * @brief Fetches a detailed list of models from NanoGPT.
     * @return A list of models with context sizes and capabilities.
     */
    std::vector<firmius::shared::ModelInfo> listModels() override;

protected:
    /**
     * @brief Returns headers with authorization.
     */
    std::map<std::string, std::string> buildHeadersForApiKey(const std::string& apiKey) override;

    /**
     * @brief Returns "reasoning_content" as used by StepFun models.
     */
    std::string getReasoningFieldName() const override;

    /**
     * @brief Override to inject NanoGPT-specific token-caching hints
     *        (top-level `caching: true` for cache-capable provider
     *        routing; `promptCaching: {enabled: true, ttl: "5m"}` for
     *        Claude-routed models that honour Anthropic-style explicit
     *        cache control).
     */
    std::string prepareRequestBody(const firmius::shared::AgentHistory &history,
                                   const firmius::provider::ProviderOptions &opts) override;
};

}

#endif

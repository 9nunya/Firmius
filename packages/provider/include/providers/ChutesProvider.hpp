#ifndef FIRMIUS_PROVIDER_CHUTES_PROVIDER_HPP
#define FIRMIUS_PROVIDER_CHUTES_PROVIDER_HPP

#include "providers/BaseOpenAIProvider.hpp"
#include <string>

namespace firmius::provider {

/**
 * @brief Provider implementation for Chutes AI (decentralized GPU inference).
 * Base URL: https://llm.chutes.ai/v1
 * Auth: cpk_ API keys or Bearer token.
 */
class ChutesProvider : public BaseOpenAIProvider {
public:
    /**
     * @brief Constructs a ChutesProvider.
     * @param apiKey The Chutes API key (cpk_ prefix) or empty to load from env.
     */
    explicit ChutesProvider(const std::string& apiKey = "");

    /**
     * @brief Fetches models from Chutes with dynamic pricing.
     */
    std::vector<firmius::shared::ModelInfo> listModels() override;

protected:
    /**
     * @brief Returns "reasoning_content" — Chutes uses both "reasoning" and
     * "reasoning_content" depending on the backend. The base class fallback
     * in processSSELine handles the "reasoning" variant automatically.
     */
    std::string getReasoningFieldName() const override;
};

}

#endif

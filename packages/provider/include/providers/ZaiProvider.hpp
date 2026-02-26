#ifndef FIRMIUS_PROVIDER_ZAI_PROVIDER_HPP
#define FIRMIUS_PROVIDER_ZAI_PROVIDER_HPP

#include "providers/BaseOpenAIProvider.hpp"
#include <string>

namespace firmius::provider {

/**
 * @brief Provider implementation for Zai.
 */
class ZaiProvider : public BaseOpenAIProvider {
public:
    /**
     * @brief Constructs a ZaiProvider.
     * @param apiKey The Zai API key.
     */
    ZaiProvider(const std::string& apiKey);

    /**
     * @brief Fetches models and defaults context window to 202k if unknown.
     */
    std::vector<firmius::shared::ModelInfo> listModels() override;
};

}

#endif

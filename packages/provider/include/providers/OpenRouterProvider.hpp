#ifndef FIRMIUS_PROVIDER_OPENROUTER_PROVIDER_HPP
#define FIRMIUS_PROVIDER_OPENROUTER_PROVIDER_HPP

#include "providers/BaseOpenAIProvider.hpp"
#include <string>

namespace firmius::provider {

/**
 * @brief Provider implementation for OpenRouter.
 */
class OpenRouterProvider : public BaseOpenAIProvider {
public:
    /**
     * @brief Constructs an OpenRouterProvider.
     * @param apiKey The OpenRouter API key.
     */
    OpenRouterProvider(const std::string& apiKey);

protected:
    /**
     * @brief Returns headers required by OpenRouter (HTTP-Referer, X-Title).
     */
    std::map<std::string, std::string> getHeaders() override;

    /**
     * @brief Returns "reasoning" field as expected by OpenRouter.
     */
    std::string getReasoningFieldName() const override;
};

}

#endif

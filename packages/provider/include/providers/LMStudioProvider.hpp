#ifndef FIRMIUS_PROVIDER_LMSTUDIO_PROVIDER_HPP
#define FIRMIUS_PROVIDER_LMSTUDIO_PROVIDER_HPP

#include "providers/BaseOpenAIProvider.hpp"
#include <string>

namespace firmius::provider {

/**
 * @brief Provider implementation for LM Studio (local LLM inference).
 * LM Studio exposes an OpenAI-compatible API at /v1/chat/completions
 * and a native model discovery endpoint at /api/v1/models.
 * Default base URL: http://localhost:1234
 * Configure via LMSTUDIO_BASE_URL environment variable or constructor parameter.
 */
class LMStudioProvider : public BaseOpenAIProvider {
public:
    /**
     * @brief Constructs an LMStudioProvider.
     * @param baseUrl Optional custom base URL. If empty, uses LMSTUDIO_BASE_URL env var or defaults to http://localhost:1234.
     */
    explicit LMStudioProvider(const std::string& baseUrl = "");
    bool isConfigured() const override;
    std::unique_ptr<APIKeyWizard> beginConnectionWizard() override;

    /**
     * @brief Fetches available models from LM Studio's native /api/v1/models endpoint.
     * Filters to only include LLM-type models (excludes embedding models).
     * @return A list of models with context sizes and capabilities.
     */
    std::vector<firmius::shared::ModelInfo> listModels() override;

protected:
    /**
     * @brief Returns minimal headers (no auth required for local inference).
     */
    std::map<std::string, std::string> buildHeadersForApiKey(const std::string& apiKey) override;
    std::string getChatUrl() const override;
};

}

#endif

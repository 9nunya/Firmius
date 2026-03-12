#ifndef FIRMIUS_PROVIDER_NANOGPT_PROVIDER_HPP
#define FIRMIUS_PROVIDER_NANOGPT_PROVIDER_HPP

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
    std::map<std::string, std::string> getHeaders() override;

    /**
     * @brief Returns "reasoning_content" as used by StepFun models.
     */
    std::string getReasoningFieldName() const override;
};

}

#endif

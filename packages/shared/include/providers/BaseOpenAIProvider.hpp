#ifndef FIRMIUS_PROVIDER_BASE_OPENAI_PROVIDER_HPP
#define FIRMIUS_PROVIDER_BASE_OPENAI_PROVIDER_HPP

#include "IProvider.hpp"
#include <string>
#include <map>
#include <vector>
#include <chrono>

namespace firmius::provider {

class BaseOpenAIProvider : public IProvider {
public:
    BaseOpenAIProvider(std::string id, const std::string& baseUrl, const std::string& apiKey);
    
    std::string getId() const override { return providerId; }

    void stream(const AgentHistory& history, const ProviderOptions& opts, 
                std::function<void(const StreamEvent&)> onEvent) override;
    
    std::vector<ModelInfo> listModels() override;

    void processSSELine(const std::string& line, std::function<void(const StreamEvent&)>& onEvent);

    std::string generateSummary(const AgentHistory& history, const std::string& compactionPrompt) override;

protected:
    std::string providerId;
    std::string baseUrl;
    std::string apiKey;

    virtual std::map<std::string, std::string> getHeaders();
    virtual std::string prepareRequestBody(const AgentHistory& history, const ProviderOptions& opts);
    virtual std::string getReasoningFieldName() const;

    /**
     * @brief Calculates estimated cost based on token usage and model pricing.
     */
    void calculateCost(AgentMetrics& metrics, const ModelInfo& model) const;

    /**
     * @brief Whether this provider supports stream_options.include_usage.
     * Override to return false for providers that don't support it.
     */
    virtual bool supportsStreamUsage() const;

    /**
     * @brief Returns the ModelInfo for a given model ID (for cost calculation).
     * Default implementation does a linear scan of listModels(). Override for caching.
     */
    virtual ModelInfo getModelInfo(const std::string& modelId);

private:
    /**
     * @brief Returns current time in milliseconds since epoch.
     */
    static std::uint64_t nowMs();

    std::vector<ModelInfo> cachedModels;
    bool modelsCached = false;
};

}

#endif

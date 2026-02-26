#ifndef FIRMIUS_PROVIDER_BASE_OPENAI_PROVIDER_HPP
#define FIRMIUS_PROVIDER_BASE_OPENAI_PROVIDER_HPP

#include "IProvider.hpp"
#include <string>
#include <map>
#include <vector>

namespace firmius::provider {

class BaseOpenAIProvider : public IProvider {
public:
    BaseOpenAIProvider(const std::string& baseUrl, const std::string& apiKey);
    
    void stream(const AgentHistory& history, const ProviderOptions& opts, 
                std::function<void(const StreamEvent&)> onEvent) override;
    
    std::vector<ModelInfo> listModels() override;

    void processSSELine(const std::string& line, std::function<void(const StreamEvent&)>& onEvent);

protected:
    virtual std::map<std::string, std::string> getHeaders();
    virtual std::string prepareRequestBody(const AgentHistory& history, const ProviderOptions& opts);
    virtual std::string getReasoningFieldName() const;

    std::string baseUrl;
    std::string apiKey;
};

}

#endif

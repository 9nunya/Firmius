#ifndef FIRMIUS_PROVIDER_GITLAWBPROVIDER_HPP
#define FIRMIUS_PROVIDER_GITLAWBPROVIDER_HPP

#include "providers/BaseOpenAIProvider.hpp"

namespace firmius::provider {

class GitlawbProvider : public BaseOpenAIProvider {
public:
    explicit GitlawbProvider();

    std::vector<shared::ModelInfo> listModels() override;
    std::string getReasoningFieldName() const override;
    bool isConfigured() const override;
    std::unique_ptr<APIKeyWizard> beginConnectionWizard() override;

protected:
    std::map<std::string, std::string> buildHeadersForApiKey(
        const std::string &apiKey) override;
};

} // namespace firmius::provider

#endif // FIRMIUS_PROVIDER_GITLAWBPROVIDER_HPP

#ifndef FIRMIUS_PROVIDER_CONFIGURABLE_OPENAI_PROVIDER_HPP
#define FIRMIUS_PROVIDER_CONFIGURABLE_OPENAI_PROVIDER_HPP

#include "ConfigLoader.hpp"
#include "providers/BaseOpenAIProvider.hpp"

namespace firmius::provider {

class ConfigurableOpenAIProvider : public BaseOpenAIProvider {
public:
  ConfigurableOpenAIProvider(const std::string &id,
                             const shared::ProviderProfileConfig &profile);

protected:
  std::map<std::string, std::string> getHeaders() override;
  std::string getReasoningFieldName() const override;
  std::string getChatUrl() const override;
  std::string getModelsUrl() const override;
  bool supportsStreamUsage() const override;
  std::string prepareRequestBody(const AgentHistory &history,
                                 const ProviderOptions &opts) override;
  std::vector<ModelInfo> listModels() override;

private:
  shared::ProviderProfileConfig profile_;
};

} // namespace firmius::provider

#endif

#ifndef FIRMIUS_PROVIDER_CONFIGURABLE_OPENAI_PROVIDER_HPP
#define FIRMIUS_PROVIDER_CONFIGURABLE_OPENAI_PROVIDER_HPP

#include "ConfigLoader.hpp"
#include "providers/BaseOpenAIProvider.hpp"

namespace firmius::provider {

class ConfigurableOpenAIProvider : public BaseOpenAIProvider {
public:
  ConfigurableOpenAIProvider(const std::string &id,
                             const shared::ProviderProfileConfig &profile);
  std::unique_ptr<APIKeyWizard> beginConnectionWizard() override;
  ModelInfo getModelInfo(const std::string &modelId) override;
  bool isConfigured() const override;
  std::optional<APIKeyAccount *>
  getAvailableAccount(const std::optional<std::string> &modelId = std::nullopt)
      override;

protected:
  std::map<std::string, std::string> getHeaders() override;
  std::map<std::string, std::string>
  buildHeadersForApiKey(const std::string &apiKey) override;
  std::string getReasoningFieldName() const override;
  std::string getChatUrl() const override;
  std::string getModelsUrl() const override;
  bool supportsStreamUsage() const override;
  std::string prepareRequestBody(const AgentHistory &history,
                                 const ProviderOptions &opts) override;
  std::vector<ModelInfo> listModels() override;

private:
  void applyModelOverrides(ModelInfo &model) const;
  shared::ProviderProfileConfig profile_;
  mutable APIKeyAccount fallbackAccount_;
};

} // namespace firmius::provider

#endif

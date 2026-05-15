#ifndef FIRMIUS_PROVIDER_CONFIGURABLE_ANTHROPIC_PROVIDER_HPP
#define FIRMIUS_PROVIDER_CONFIGURABLE_ANTHROPIC_PROVIDER_HPP

#include "ConfigLoader.hpp"
#include "providers/BaseAnthropicProvider.hpp"

namespace firmius::provider {

class ConfigurableAnthropicProvider : public BaseAnthropicProvider {
public:
  ConfigurableAnthropicProvider(const std::string &id,
                                const shared::ProviderProfileConfig &profile);
  std::unique_ptr<APIKeyWizard> beginConnectionWizard() override;
  ModelInfo getModelInfo(const std::string &modelId) override;
  bool isConfigured() const override;
  std::optional<APIKeyAccount *>
  getAvailableAccount(const std::optional<std::string> &modelId = std::nullopt)
      override;

protected:
  std::map<std::string, std::string> getHeaders() override;
  std::string getMessagesUrl() const override;
  std::string getAnthropicBetaHeader() const override;
  std::string prepareRequestBody(const AgentHistory &history,
                                 const ProviderOptions &opts) override;
  std::vector<ModelInfo> listModels() override;

private:
  std::string getModelsUrl() const;
  void applyModelOverrides(ModelInfo &model) const;

private:
  shared::ProviderProfileConfig profile_;
  mutable APIKeyAccount fallbackAccount_;
};

} // namespace firmius::provider

#endif

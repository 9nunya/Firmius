#ifndef FIRMIUS_PROVIDER_CONFIGURABLE_ANTHROPIC_PROVIDER_HPP
#define FIRMIUS_PROVIDER_CONFIGURABLE_ANTHROPIC_PROVIDER_HPP

#include "ConfigLoader.hpp"
#include "providers/BaseAnthropicProvider.hpp"

namespace firmius::provider {

class ConfigurableAnthropicProvider : public BaseAnthropicProvider {
public:
  ConfigurableAnthropicProvider(const std::string &id,
                                const shared::ProviderProfileConfig &profile);

protected:
  std::map<std::string, std::string> getHeaders() override;
  std::string getMessagesUrl() const override;
  std::string getAnthropicBetaHeader() const override;
  std::string prepareRequestBody(const AgentHistory &history,
                                 const ProviderOptions &opts) override;
  std::vector<ModelInfo> listModels() override;

private:
  std::string getModelsUrl() const;

private:
  shared::ProviderProfileConfig profile_;
};

} // namespace firmius::provider

#endif

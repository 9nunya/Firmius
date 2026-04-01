#ifndef FIRMIUS_PROVIDER_NVIDIA_PROVIDER_HPP
#define FIRMIUS_PROVIDER_NVIDIA_PROVIDER_HPP

#include "providers/BaseOpenAIProvider.hpp"

#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace firmius::provider {

/**
 * @brief Provider implementation for NVIDIA NIM's OpenAI-compatible API.
 */
class NvidiaProvider : public BaseOpenAIProvider {
public:
  explicit NvidiaProvider(const std::string &apiKey = "");

  std::vector<firmius::shared::ModelInfo> listModels() override;
  void discoverModels(
      std::function<void(const firmius::shared::ModelInfo &)> onModel) override;

protected:
  virtual std::string fetchUrl(
      const std::string &url,
      const std::map<std::string, std::string> &headers) const;

private:
  std::vector<std::string> fetchModelIds();
  std::optional<firmius::shared::ModelInfo>
  fetchModelInfo(const std::string &modelId);

  mutable std::mutex modelCacheMutex_;
  mutable std::vector<firmius::shared::ModelInfo> modelCache_;
  mutable bool modelCacheLoaded_ = false;
};

} // namespace firmius::provider

#endif

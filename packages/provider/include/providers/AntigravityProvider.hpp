#pragma once

#include "providers/BaseOAuthProvider.hpp"
#include <map>
#include <rapidjson/document.h>
#include <memory>

namespace firmius::provider {

class AntigravityProvider : public BaseOAuthProvider {
public:
  AntigravityProvider();
  ~AntigravityProvider() override = default;

  // ------------------------------------------------------------------------
  // IProvider interface overrides
  // ------------------------------------------------------------------------

  std::vector<ModelInfo> listModels() override;
  ModelInfo getModelInfo(const std::string &modelId) override;

  void stream(const AgentHistory &history, const ProviderOptions &opts,
              std::function<void(const StreamEvent &)> onEvent) override;

  void generateSummary(const std::string &modelId, const AgentHistory &history,
                       const std::string &compactionPrompt,
                       std::function<void(const StreamEvent &)> onEvent,
                       std::atomic<bool> *abortSignal = nullptr) override;

  // ------------------------------------------------------------------------
  // OAuth implementation overrides
  // ------------------------------------------------------------------------

  std::unique_ptr<OAuthWizard> beginConnectionWizard() override;
  bool refreshAccessToken(OAuthAccount &acc) override;

  void refreshQuotas() override;
  std::map<std::string, std::vector<QuotaBucket>> getAllQuotas() const override;

  std::optional<OAuthAccount *> getAvailableAccount(
      const std::optional<std::string> &modelId = std::nullopt) override;

  // Parses chunks arriving from the CURL callback.
  void processSSELine(const std::string &line,
                      std::function<void(const StreamEvent &)> &onEvent);

  // Queries Antigravity to get quota definitions for the given account
  void fetchAndStoreQuotas(OAuthAccount &acc);

private:
  // Fetches the managed project ID from Antigravity
  std::string fetchManagedProject(OAuthAccount &acc);
  // Resolves a usable project ID for streaming requests.
  std::string resolveProjectIdForAccount(OAuthAccount &acc, bool forceRefresh);

  static std::map<std::string, ModelInfo> getStaticModels();

  // Sends the internal Antigravity proxy request (v1internal:streamGenerateContent)
  void executeStreamRequest(OAuthAccount &acc, const AgentHistory &history,
                            const ProviderOptions &opts,
                            std::function<void(const StreamEvent &)> &onEvent);

  static size_t sseWriteCallback(char *ptr, size_t size, size_t nmemb, void *userdata);
};

} // namespace firmius::provider

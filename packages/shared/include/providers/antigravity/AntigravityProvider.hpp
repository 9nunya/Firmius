#pragma once

#include "providers/oauth/BaseOAuthProvider.hpp"
#include <map>
#include <rapidjson/document.h>

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

  // Parses chunks arriving from the CURL callback. The payload is heavily
  // nested in `response`
  void processSSELine(const std::string &line,
                      std::function<void(const StreamEvent &)> &onEvent);

  // Queries Antigravity to get quota definitions for the given account and
  // stores them in acc.metadata
  void fetchAndStoreQuotas(OAuthAccount &acc);

private:
  // Fetches the managed project ID from Antigravity
  std::string fetchManagedProject(OAuthAccount &acc);

  static std::map<std::string, ModelInfo> getStaticModels();

  // Sends the internal Antigravity proxy request
  // (v1internal:streamGenerateContent)
  void executeStreamRequest(OAuthAccount &acc, const AgentHistory &history,
                            const ProviderOptions &opts,
                            std::function<void(const StreamEvent &)> &onEvent);

  std::string prepareRequestBody(const AgentHistory &history,
                                 const ProviderOptions &opts,
                                 const OAuthAccount &acc,
                                 const std::string &effectiveModel,
                                 const std::string &effectiveProjectId,
                                 const std::string &signatureSessionKey,
                                 rapidjson::Document::AllocatorType &a);

  static rapidjson::Value toGeminiSchema(const std::string &inputSchema,
                                         rapidjson::Document::AllocatorType &a);

  static size_t headerCallback(char *ptr, size_t size, size_t nmemb,
                               void *userdata);
};

} // namespace firmius::provider

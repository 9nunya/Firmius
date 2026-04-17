#ifndef FIRMIUS_PROVIDER_KIROPROVIDER_HPP
#define FIRMIUS_PROVIDER_KIROPROVIDER_HPP

#include "providers/BaseOAuthProvider.hpp"
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace firmius::provider {

class KiroProvider : public BaseOAuthProvider {
public:
  KiroProvider();
  ~KiroProvider() override;

  // IProvider interface
  std::vector<firmius::shared::ModelInfo> listModels() override;
  firmius::shared::ModelInfo getModelInfo(const std::string &modelId) override;
  void stream(const firmius::shared::AgentHistory &history,
              const ProviderOptions &opts,
              std::function<void(const firmius::shared::StreamEvent &)> onEvent) override;
  void generateSummary(const std::string &modelId,
                       const firmius::shared::AgentHistory &history,
                       const std::string &compactionPrompt,
                       std::function<void(const firmius::shared::StreamEvent &)> onEvent,
                       std::atomic<bool> *abortSignal = nullptr) override;

  // OAuth-specific
  std::unique_ptr<firmius::OAuthWizard> beginConnectionWizard() override;
  bool refreshAccessToken(firmius::shared::OAuthAccount &acc) override;
  void refreshQuotas() override;
  std::map<std::string, std::vector<firmius::shared::QuotaBucket>> getAllQuotas() const override;
  std::optional<firmius::shared::OAuthAccount> getAvailableAccount(
      const std::optional<std::string> &modelId = std::nullopt) override;

  // HTTP helpers (public for OAuth wizard)
  static std::string buildUrl(const std::string &template_url, const std::string &region);
  std::string buildCodeWhispererRequestForTest(
      const firmius::shared::AgentHistory &history,
      const std::string &modelId,
      const firmius::shared::OAuthAccount &acc,
      const ProviderOptions &opts) {
    return buildCodeWhispererRequest(history, modelId, acc, opts);
  }
  static size_t sseWriteCallbackForTest(char *ptr, size_t size, size_t nmemb,
                                        void *userdata) {
    return sseWriteCallback(ptr, size, nmemb, userdata);
  }

  struct StreamContext {
    KiroProvider *provider;
    std::function<void(const firmius::shared::StreamEvent &)> *onEvent;
    std::string buffer;
    size_t readOffset = 0;
    std::string activeToolUseId;
    std::string contentBuffer;
    bool inThinking = false;
    bool thinkingExtracted = false;
    std::atomic<bool> *abortSignal = nullptr;
    firmius::shared::AgentMetrics metrics;
    bool metricsReceived = false;
    bool doneReceived = false;
  };

private:
  // Constants
  static constexpr const char *kProviderId = "kiro";
  static constexpr const char *kDefaultRegion = "us-east-1";
  static constexpr const char *kSsoOidcEndpoint = "https://oidc.{{region}}.amazonaws.com";
  static constexpr const char *kApiEndpoint = "https://q.{{region}}.amazonaws.com/generateAssistantResponse";
  static constexpr const char *kUsageLimitsEndpoint = "https://q.{{region}}.amazonaws.com/getUsageLimits";
  static constexpr const char *kDesktopRefreshEndpoint = "https://prod.{{region}}.auth.desktop.kiro.dev/refreshToken";
  static constexpr const char *kBuilderIdStartUrl = "https://view.awsapps.com/start";

  // Model data
  struct KiroModel {
    std::string id;
    std::string displayName;
    double creditMultiplier;
    std::uint32_t contextWindow;
    std::uint32_t maxOutput;
    std::vector<std::string> modalities;
    bool supportsThinking = false;
  };
  static std::vector<KiroModel> getKiroModels();
  static std::string resolveModelId(const std::string &modelId);

  static size_t sseWriteCallback(char *ptr, size_t size, size_t nmemb, void *userdata);

  // Token management
  bool refreshTokenIDC(firmius::shared::OAuthAccount &acc);
  bool refreshTokenDesktop(firmius::shared::OAuthAccount &acc);

  // Quota
  bool fetchUsageLimits(firmius::shared::OAuthAccount &acc);

  // Request building
  std::string buildCodeWhispererRequest(
      const firmius::shared::AgentHistory &history,
      const std::string &modelId,
      const firmius::shared::OAuthAccount &acc,
      const ProviderOptions &opts);

};

} // namespace firmius::provider

#endif

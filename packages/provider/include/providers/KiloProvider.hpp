#ifndef FIRMIUS_PROVIDER_KILO_PROVIDER_HPP
#define FIRMIUS_PROVIDER_KILO_PROVIDER_HPP

#include "providers/BaseOpenAIProvider.hpp"
#include <atomic>
#include <mutex>
#include <thread>

namespace firmius::provider {

/**
 * @brief OAuth device flow wizard for Kilo provider.
 * Guides user through device authorization: request device code, open URL,
 * poll until user approves, capture bearer token.
 */
class KiloAPIKeyWizard : public APIKeyWizard {
public:
  KiloAPIKeyWizard();
  ~KiloAPIKeyWizard() override;

  // APIKeyWizard interface
  std::optional<firmius::WizardPrompt> nextPrompt() override;
  void submitAnswer(const std::string &answer) override;
  bool isComplete() const override;
  bool finalizeExchange(std::string &outApiKey, std::string &outErrorMessage) override;
  std::string getFinalMessage() const override;

private:
  std::string token_;

  struct DeviceAuthResponse {
    std::string code;
    std::string verificationUrl;
    int expiresIn; // seconds
  };

  struct TokenResponse {
    std::string accessToken;
    std::string refreshToken;
    int expiresIn;
  };

  std::string prompt_;
  bool promptShown_ = false;
  bool isComplete_ = false;
  std::string errorMessage_;

  std::thread pollingThread_;
  std::atomic<bool> stopPolling_{false};
  std::atomic<bool> pollingDone_{false};

  DeviceAuthResponse deviceResponse_;
  std::string pollingError_;

  void startPolling();
  void stopPolling();
  void pollLoop();

  DeviceAuthResponse requestDeviceCode();
  std::optional<TokenResponse> pollForToken(const std::string &code, int expiresIn);
};

/**
 * @brief Kilo AI Gateway provider.
 *
 * Authenticates via OAuth device flow. Falls back to anonymous access for free models.
 */
class KiloProvider : public BaseOpenAIProvider {
public:
  KiloProvider();
  ~KiloProvider() override;

  // IProvider overrides (stream inherited)
  std::vector<ModelInfo> listModels() override;
  ModelInfo getModelInfo(const std::string &modelId) override;

  // API key wizard
  std::unique_ptr<APIKeyWizard> beginConnectionWizard() override;

  // Quota tracking
  bool supportsQuotaTracking() const override { return true; }
  void refreshQuotas() override;
  std::map<std::string, std::vector<QuotaBucket>> getAllQuotas() const override;

  // Account selection with anonymous fallback
  std::optional<APIKeyAccount *> getAvailableAccount(
      const std::optional<std::string> &modelId = std::nullopt) override;

  // Stream with cost accounting
  void stream(const AgentHistory &history, const ProviderOptions &opts,
              std::function<void(const StreamEvent &)> onEvent) override;

protected:
  std::map<std::string, std::string> buildHeadersForApiKey(
      const std::string &apiKey) override;

private:
  struct CachedModelInfo {
    std::string id;
    std::string family;
    std::uint32_t contextWindow = 0;
    std::uint32_t maxOutputTokens = 0;
    bool supportsImages = false;
    bool supportsTools = false;
    bool supportsReasoning = false;
    bool supportsTemperature = false;
    double pricePer1MInput = 0.0;
    double pricePer1MOutput = 0.0;
    double pricePer1MCacheRead = 0.0;
    double pricePer1MCacheWrite = 0.0;
  };

  mutable std::mutex modelsMutex_;
  std::vector<CachedModelInfo> modelCache_;
  std::string baseUrl_;

  // Helpers
  bool isFreeModel(const std::string &modelId) const;
  bool isAnonymous(const APIKeyAccount &acc) const;
  bool isExhausted(const APIKeyAccount &acc,
                   const std::optional<std::string> &modelId) const;
  void updateSpending(APIKeyAccount &acc, const std::string &modelId,
                      std::uint64_t promptTokens, std::uint64_t completionTokens);
  double getModelPricePer1MInput(const std::string &modelId);
  double getModelPricePer1MOutput(const std::string &modelId);
  void ensureModelsLoaded();
  std::string extractFamily(const std::string &modelId) const;

  // Anonymous account
  APIKeyAccount &getOrCreateAnonymousAccount();

  // Metadata keys
  static constexpr const char *kMetaAccumulatedCost = "accumulated_cost_microdollars";
  static constexpr const char *kMetaBudget = "kilo_budget_microdollars";
  static constexpr const char *kMetaAnonymousFlag = "kilo_is_anonymous";

  // Constants
  static constexpr const char *kAnonymousIdentifier = "Anonymous (free models only)";
  static constexpr const char *kAnonymousToken = "anonymous";
  static constexpr int kPollIntervalMs = 3000;
};

} // namespace firmius::provider

#endif // FIRMIUS_PROVIDER_KILO_PROVIDER_HPP

#ifndef FIRMIUS_PROVIDER_BASEAPIKEYPROVIDER_HPP
#define FIRMIUS_PROVIDER_BASEAPIKEYPROVIDER_HPP

#include "Enums.hpp"
#include "IProvider.hpp"
#include "providers/OAuthWizard.hpp"
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace firmius::provider {

using firmius::shared::APIKeyAccount;
using firmius::shared::QuotaBucket;

/**
 * @brief Abstract wizard interface for API key connection flows.
 */
class APIKeyWizard {
public:
  virtual ~APIKeyWizard() = default;
  virtual std::optional<firmius::WizardPrompt> nextPrompt() = 0;
  virtual void submitAnswer(const std::string &answer) = 0;
  virtual bool isComplete() const = 0;
  virtual bool finalizeExchange(std::string &outApiKey,
                                std::string &outErrorMessage) = 0;
  virtual std::string getFinalMessage() const = 0;
};

/**
 * @brief Simple wizard for direct API key input.
 */
class SimpleAPIKeyWizard : public APIKeyWizard {
public:
  SimpleAPIKeyWizard() {
    prompt_.message = "Enter your API key:";
    prompt_.placeholder = "sk-...";
    prompt_.submitLabel = "Save Key";
    prompt_.isSecret = true;
  }

  std::optional<firmius::WizardPrompt> nextPrompt() override {
    if (!promptShown_) {
      promptShown_ = true;
      return prompt_;
    }
    return std::nullopt;
  }

  void submitAnswer(const std::string &answer) override {
    apiKey_ = answer;
    isComplete_ = true;
  }

  bool isComplete() const override { return isComplete_; }

  bool finalizeExchange(std::string &outApiKey,
                        std::string &outErrorMessage) override {
    if (apiKey_.empty()) {
      outErrorMessage = "API key cannot be empty.";
      return false;
    }
    outApiKey = apiKey_;
    return true;
  }

  std::string getFinalMessage() const override {
    return "API key successfully added!";
  }

private:
  firmius::WizardPrompt prompt_;
  bool promptShown_ = false;
  std::string apiKey_;
  bool isComplete_ = false;
};

/**
 * @brief Base class for providers that use API key authentication.
 *
 * Supports multiple API keys with rotation and rate limit backoff.
 * Keys are persisted to ~/.firmius/keys.json
 */
class BaseAPIKeyProvider : public IProvider {
public:
  explicit BaseAPIKeyProvider(std::string providerId);
  virtual ~BaseAPIKeyProvider() = default;

  // IProvider interface
  std::string getId() const override;
  ProviderType getProviderType() const override;
  bool isConfigured() const override;

  // IProvider interface - stream, listModels, getModelInfo, generateSummary are
  // implemented by subclasses
  bool supportsStreamUsage() const { return true; }

  /**
   * @brief Whether this API-key provider exposes quota/usage tracking.
   */
  virtual bool supportsQuotaTracking() const { return false; }

  /**
   * @brief Refresh quota information for all configured keys.
   */
  virtual void refreshQuotas() {}

  /**
   * @brief Returns aggregated quotas for all configured keys.
   */
  virtual std::map<std::string, std::vector<QuotaBucket>> getAllQuotas() const {
    return {};
  }

  /**
   * @brief Get all configured API key accounts.
   */
  std::vector<APIKeyAccount> getAccounts() const;

  /**
   * @brief Get number of configured API keys.
   */
  size_t getAccountCount() const;

  /**
   * @brief Add a new API key account.
   */
  void addAccount(const APIKeyAccount &acc);
  void addApiKey(const std::string &apiKey);

  /**
   * @brief Delete an API key account by identifier.
   */
  void deleteAccount(const std::string &identifier);

  /**
   * @brief Get an available (non-rate-limited) API key account.
   * @param modelId Optional model ID for provider-specific selection logic.
   * @return Pointer to available account, or nullopt if all are rate-limited.
   */
  virtual std::optional<APIKeyAccount *> getAvailableAccount(
      const std::optional<std::string> &modelId = std::nullopt);

  /**
   * @brief Mark an account as rate-limited for a specified duration.
   */
  void markAccountRateLimited(APIKeyAccount &acc, int backoffSeconds);

  /**
   * @brief Begin the API key connection wizard.
   * @return Wizard instance or nullptr if provider doesn't support interactive
   * setup.
   */
  virtual std::unique_ptr<APIKeyWizard> beginConnectionWizard() = 0;

protected:
  std::string providerId_;
  mutable std::recursive_mutex accountsMutex_;
  std::vector<APIKeyAccount> accounts_;
  std::atomic<int> lastUsedIndex_{0};

  /**
   * @brief Load accounts from keys.json.
   */
  void loadAccounts();

  /**
   * @brief Save accounts to keys.json.
   */
  void saveAccounts();

  /**
   * @brief Get current time in seconds since epoch.
   */
  static int64_t getNowSeconds();

  /**
   * @brief Extract first 5 characters of an API key for safe display.
   */
  static std::string extractKeyPrefix(const std::string &apiKey);

  /**
   * @brief Generate a display identifier for a new key.
   */
  std::string generateIdentifier() const;
};

} // namespace firmius::provider

#endif // FIRMIUS_PROVIDER_BASEAPIKEYPROVIDER_HPP

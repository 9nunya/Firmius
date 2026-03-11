#ifndef FIRMIUS_PROVIDER_BASE_API_KEY_PROVIDER_HPP
#define FIRMIUS_PROVIDER_BASE_API_KEY_PROVIDER_HPP

#include "IProvider.hpp"
#include <string>
#include <vector>
#include <optional>
#include <mutex>
#include <memory>

namespace firmius::provider {

/**
 * @brief Represents an API key account with rate limiting state.
 */
struct APIKeyAccount {
  std::string identifier;       // Display name (e.g., "Key #1", "Key #2")
  std::string keyPrefix;        // First 5 chars of the API key for display
  std::string apiKey;           // The actual API key
  bool rateLimited = false;
  int64_t backoffUntil = 0;     // Epoch seconds when backoff expires
  std::map<std::string, std::string> metadata; // Provider-specific metadata

  std::string getIdentifier() const { return identifier; }
  std::string getMaskedDisplay() const { return keyPrefix + "..."; }
};

/**
 * @brief Abstract wizard interface for API key connection flows.
 */
class APIKeyWizard {
public:
  virtual ~APIKeyWizard() = default;
  virtual std::optional<std::string> nextPrompt() = 0;
  virtual void submitAnswer(const std::string &answer) = 0;
  virtual bool isComplete() const = 0;
  virtual bool finalizeExchange(std::string &outApiKey,
                                std::string &outErrorMessage) = 0;
  virtual std::string getFinalMessage() const = 0;
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
  bool supportsStreamUsage() const override;

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

  /**
   * @brief Delete an API key account by identifier.
   */
  void deleteAccount(const std::string &identifier);

  /**
   * @brief Get an available (non-rate-limited) API key account.
   * @param modelId Optional model ID for provider-specific selection logic.
   * @return Pointer to available account, or nullopt if all are rate-limited.
   */
  std::optional<APIKeyAccount *> getAvailableAccount(
      const std::optional<std::string> &modelId = std::nullopt);

  /**
   * @brief Mark an account as rate-limited for a specified duration.
   */
  void markAccountRateLimited(APIKeyAccount &acc, int backoffSeconds);

  /**
   * @brief Begin the API key connection wizard.
   * @return Wizard instance or nullptr if provider doesn't support interactive setup.
   */
  virtual std::unique_ptr<APIKeyWizard> beginConnectionWizard() = 0;

protected:
  std::string providerId_;
  mutable std::recursive_mutex accountsMutex_;
  std::vector<APIKeyAccount> accounts_;
  int lastUsedIndex_ = 0;

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

#endif // FIRMIUS_PROVIDER_BASE_API_KEY_PROVIDER_HPP

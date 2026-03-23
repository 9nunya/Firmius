#pragma once

#include "IProvider.hpp"
#include "providers/OAuthWizard.hpp"
#include <chrono>
#include <map> // Added for std::map
#include <memory>
#include <string>
#include <mutex>
#include <vector>
#include <thread>
#include <atomic>
#include <condition_variable>

namespace firmius::provider {

using namespace firmius::shared;

class BaseOAuthProvider : public IProvider {
public:
  explicit BaseOAuthProvider(std::string providerId);
  virtual ~BaseOAuthProvider();

  // ------------------------------------------------------------------------
  // IProvider interface overrides
  // ------------------------------------------------------------------------
  std::string getId() const override;
  ProviderType getProviderType() const override;
  bool isConfigured() const override;
  bool supportsStreamUsage() const;

  // Abstract definitions remaining from IProvider (subclasses must implement):
  // stream(...)
  // listModels()
  // getModelInfo(...)
  // generateSummary(...)

  // ------------------------------------------------------------------------
  // OAuth specific interface
  // ------------------------------------------------------------------------

  // Returns a configured wizard to step the user through the interactive CLI
  // phase
  virtual std::unique_ptr<OAuthWizard> beginConnectionWizard() = 0;

  // Exchange the given accounts refresh token for an access token
  virtual bool refreshAccessToken(OAuthAccount &acc) = 0;

  // Load available accounts from ~/.firmius/oauth.json for this provider
  void loadAccounts();
  void saveAccounts();

  // Gets the current available account, handling rotation and skipping
  // rate-limited ones. If modelId is provided, subclasses can use it to
  // check specific quotas.
  virtual std::optional<OAuthAccount *>
  getAvailableAccount(const std::optional<std::string> &modelId = std::nullopt);

  void addAccount(const OAuthAccount &acc);
  void deleteAccount(const std::string &identifier);

  const std::vector<OAuthAccount> &getAccounts() const { return accounts_; }

  /**
   * @brief Returns aggregated quotas for all accounts.
   * @return Map of account identifier to its quota buckets.
   */
  virtual std::map<std::string, std::vector<QuotaBucket>> getAllQuotas() const {
    return {};
  }

  /**
   * @brief Triggers a refresh of quotas for all accounts.
   */
  virtual void refreshQuotas() {}

protected:
  std::string providerId_;
  std::vector<OAuthAccount> accounts_;
  mutable std::recursive_mutex accountsMutex_;
  int lastUsedIndex_ = -1;

  std::thread quotaRefreshThread_;
  std::atomic<bool> stopQuotaRefresh_{false};
  std::mutex quotaRefreshMutex_;
  std::condition_variable quotaRefreshCv_;

  void startBackgroundQuotaRefresh();
  void stopBackgroundQuotaRefresh();

  bool isTokenExpired(const OAuthAccount &acc) const;
  void markAccountRateLimited(OAuthAccount &acc, int backoffSeconds);
};

} // namespace firmius::provider

#ifndef FIRMIUS_PROVIDER_WINDSURF_PROVIDER_HPP
#define FIRMIUS_PROVIDER_WINDSURF_PROVIDER_HPP

#include "providers/BaseOAuthProvider.hpp"
#include "providers/WindsurfModels.hpp"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace firmius::provider {
class WindsurfLspManager;
}

namespace firmius::provider {

/**
 * @brief Windsurf / Codeium provider.
 *
 * Authenticates via the same web OAuth flow used by the official
 * windsurf.vim / windsurf.nvim / JetBrains plugins:
 *
 *   1. Open the user's browser at https://windsurf.com/profile?response_type=token...
 *      pointing redirect_uri at a localhost loopback server we spin up. If the
 *      Windsurf portal accepts the loopback redirect we capture the JWT
 *      automatically (zero paste). Otherwise we fall back to a paste-back
 *      wizard prompt.
 *   2. Exchange the JWT via POST https://api.codeium.com/register_user/
 *      → response yields a long-lived `api_key` plus user metadata.
 *   3. Persist the api_key + JWT + email/name in ~/.firmius/oauth.json under
 *      the "windsurf" provider key.
 *
 * Streaming chat goes directly to https://server.codeium.com over gRPC
 * (HTTP/2 + protobuf, no local Windsurf desktop install required) using the
 * api_key for auth.
 *
 * Quotas: refreshQuotas() polls Codeium cloud user-status / usage-limits
 * endpoints every 5 minutes (driven by BaseOAuthProvider's background thread)
 * and exposes per-account QuotaBuckets — plan tier, prompt credits, flow-action
 * credits — to the existing Firmius quota TUI.
 */
class WindsurfProvider : public BaseOAuthProvider {
public:
  WindsurfProvider();
  ~WindsurfProvider() override;

  // ----- IProvider -----------------------------------------------------------
  std::vector<firmius::shared::ModelInfo> listModels() override;
  void discoverModels(
      std::function<void(const firmius::shared::ModelInfo &)> onModel) override;
  firmius::shared::ModelInfo getModelInfo(const std::string &modelId) override;
  void stream(const firmius::shared::AgentHistory &history,
              const ProviderOptions &opts,
              std::function<void(const firmius::shared::StreamEvent &)>
                  onEvent) override;
  void generateSummary(const std::string &modelId,
                       const firmius::shared::AgentHistory &history,
                       const std::string &compactionPrompt,
                       std::function<void(const firmius::shared::StreamEvent &)>
                           onEvent,
                       std::atomic<bool> *abortSignal = nullptr) override;

  // ----- BaseOAuthProvider ---------------------------------------------------
  std::unique_ptr<firmius::OAuthWizard> beginConnectionWizard() override;
  bool refreshAccessToken(firmius::shared::OAuthAccount &acc) override;
  void refreshQuotas() override;
  std::map<std::string, std::vector<firmius::shared::QuotaBucket>>
  getAllQuotas() const override;

  // ----- Windsurf-specific (publicly exposed for wizard + tests) ------------

  /**
   * @brief Exchange a Codeium/Windsurf Firebase ID token for a long-lived
   * api_key via POST https://api.codeium.com/register_user/ (returns true on
   * success and populates outAcc.identifier/accessToken/refreshToken).
   *
   * @param firebaseIdToken JWT obtained from the OAuth flow.
   * @param outAcc          Account to populate with api_key + metadata.
   * @param outError        Human-readable error if the call fails.
   */
  bool exchangeFirebaseIdToken(const std::string &firebaseIdToken,
                               firmius::shared::OAuthAccount &outAcc,
                               std::string &outError);

  /**
   * @brief Discover available models for a given account against the cloud.
   * Merges discovered enums/names into our runtime model cache. Safe to call
   * on a background thread.
   *
   * @return Number of models discovered (or 0 on error).
   */
  std::size_t fetchAndMergeModels(const firmius::shared::OAuthAccount &acc);

  static constexpr const char *kProviderId = "windsurf";

  // Stored model-cache entry merged from static fallback + cloud discovery.
  // Exposed at class scope so anonymous-namespace helpers in the
  // implementation TU can reference it without violating access rules.
  struct CachedModel {
    int enumValue = 0; // optional; 0 for newly added models
    std::string canonicalId;
    std::string displayName;
    std::vector<std::string> variants;
    std::uint32_t contextWindow = 200000;
    std::uint32_t maxOutput = 8192;
    bool supportsReasoning = false;
    bool supportsImages = false;
    double creditMultiplier = 1.0;
    // Pricing in USD per 1M tokens, parsed from
    // `ClientModelConfig.model_dimensions`.
    double pricePer1MInput = 0.0;
    double pricePer1MOutput = 0.0;
    double pricePer1MCacheRead = 0.0;
    double pricePer1MCacheWrite = 0.0;
    bool fromDiscovery = false;
  };

  // Quota state per account, populated by refreshQuotas.
  struct AccountQuota {
    std::string planTier; // "Free" | "Pro" | "Pro Ultimate" | "Teams" | etc.
    std::int64_t dailyUsed = 0;
    std::int64_t dailyLimit = 0;
    std::int64_t dailyResetEpochSeconds = 0;
    std::int64_t weeklyUsed = 0;
    std::int64_t weeklyLimit = 0;
    std::int64_t weeklyResetEpochSeconds = 0;
    double extraUsageBalanceUsd = 0.0;
    std::int64_t promptCreditsUsed = 0;
    std::int64_t promptCreditsLimit = 0;
    std::int64_t flowActionsUsed = 0;
    std::int64_t flowActionsLimit = 0;
    std::int64_t resetEpochSeconds = 0;
    std::string subscriptionExpiry;
    std::map<std::string, std::pair<std::int64_t, std::int64_t>> perModel;
  };

private:
  // Persistent model cache I/O.
  std::filesystem::path modelCachePath() const;
  void loadModelCache();
  void saveModelCache() const;

  // Cloud discovery + helpers.
  bool fetchUserStatus(const firmius::shared::OAuthAccount &acc,
                       AccountQuota &outQuota, std::string &outError);
  bool fetchUsageLimits(const firmius::shared::OAuthAccount &acc,
                        AccountQuota &outQuota, std::string &outError);

  // gRPC chat streaming (returns once the stream completes or aborts).
  bool streamChatGrpc(
      const firmius::shared::OAuthAccount &acc,
      const windsurf::ResolvedModel &model,
      const firmius::shared::AgentHistory &history,
      const ProviderOptions &opts,
      std::function<void(const firmius::shared::StreamEvent &)> onEvent);

  // Local-LSP streaming. Talks to a running Windsurf
  // language_server_linux_x64 (or _macos / _windows) via Connect-RPC over
  // localhost. Returns true if the stream completed successfully (any LLM
  // response delivered). Caller must have already verified that a local LSP
  // is reachable; on failure this method emits a StreamError + StreamDone
  // and returns false.
  //
  // cascade_ids are reused per-thread (preserving prompt-cache hits across
  // model switches and across thread reloads) by persisting the
  // threadId → cascade_id mapping in `~/.firmius/windsurf_cascades.json`.
  bool streamChatLocal(
      const firmius::shared::OAuthAccount &acc,
      const std::string &modelUid,
      int modelEnum,
      const firmius::shared::AgentHistory &history,
      const ProviderOptions &opts,
      std::function<void(const firmius::shared::StreamEvent &)> onEvent);

  // Local-LSP cascade ID persistence.
  //
  // Each entry remembers the cascade_id, the LSP's CSRF token at the time of
  // mint, and the absolute trajectory step offset already consumed by Firmius.
  // Cascade IDs are scoped to one language_server process lifetime (the LSP
  // forgets them on restart), so when we boot Firmius against a fresh LSP whose
  // CSRF doesn't match what we stored, we treat the mapping as cold and remint
  // a new cascade. The step offset is equally important: reused cascades contain
  // prior turns in their trajectory, so polling from step 0 on every prompt
  // replays old assistant text/tool calls as fresh output.
  struct CascadePersist {
    std::string cascadeId;
    std::string csrf;  // empty for legacy v0 entries
    std::uint32_t stepOffset = 0;
  };

  // Looks up a cascade for `threadId`. Returns an empty entry if there is no
  // mapping, or if the stored entry's csrf doesn't match `currentCsrf` (caller
  // will mint a fresh one and call setCascadeForThread).
  CascadePersist getCascadeForThread(const std::string &threadId,
                                     const std::string &currentCsrf);
  void setCascadeForThread(const std::string &threadId,
                           const std::string &cascadeId,
                           const std::string &csrf,
                           std::uint32_t stepOffset = 0);
  void setCascadeStepOffsetForThread(const std::string &threadId,
                                     std::uint32_t stepOffset);

  std::filesystem::path cascadeMapPath() const;
  void loadCascadeMap();
  void saveCascadeMap() const;

  // Recursive because fetchAndMergeModels() holds this lock while calling
  // saveModelCache(), which acquires it again to serialize the JSON writer.
  mutable std::recursive_mutex modelMutex_;
  std::vector<CachedModel> models_;

  mutable std::mutex cascadeMutex_;
  std::map<std::string, CascadePersist> cascadeByThread_; // threadId → {cascade_id, csrf}

  mutable std::mutex quotaMutex_;
  std::map<std::string, AccountQuota> quotas_; // keyed by account identifier

  // Background discovery worker (owned, joinable). Started lazily on first
  // listModels() and joined in the destructor so we never leak a curl handle.
  std::mutex discoveryMutex_;
  std::thread discoveryThread_;
  std::atomic<bool> discoveryStarted_{false};
  std::atomic<bool> shutdownRequested_{false};

  // Owns the per-account language_server child processes. v2 path: every
  // chat goes through a child WE spawn so the api_key stays under our
  // control instead of being silently overridden by whatever account the
  // user happens to be logged into in the running Windsurf IDE. Created
  // lazily on first chat to avoid spawning until we actually need to.
  mutable std::mutex lspManagerMutex_;
  std::unique_ptr<WindsurfLspManager> lspManager_;
};

} // namespace firmius::provider

#endif

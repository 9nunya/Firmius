#ifndef FIRMIUS_PROVIDER_KIROPROVIDER_HPP
#define FIRMIUS_PROVIDER_KIROPROVIDER_HPP

#include "providers/BaseOAuthProvider.hpp"
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace firmius::provider {

/**
 * @brief Kiro subscription tier as returned by the Kiro/Q Developer backend.
 *
 * Tiers are ordered: every higher tier is a strict superset of the lower one
 * for purposes of model availability. We use the numeric ordering (`<` / `>=`)
 * to gate models — see KiroProvider::accountTierMeetsModelMinimum.
 */
enum class KiroTier : std::uint8_t {
  Unknown = 0, ///< Tier could not yet be resolved from the live API.
  Free = 1,    ///< Builder ID / social login Free tier.
  Pro = 2,     ///< Paid Pro tier.
  ProPlus = 3, ///< Paid Pro+ tier.
  Power = 4,   ///< Paid Power tier.
};

std::string kiroTierToString(KiroTier tier);
KiroTier kiroTierFromString(const std::string &value);

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

  // Tier helpers (public so the TUI / tests can use them)
  static KiroTier accountTier(const firmius::shared::OAuthAccount &acc);
  static bool accountTierMeetsModelMinimum(KiroTier accountTier,
                                           const std::string &modelId);
  static KiroTier modelMinimumTier(const std::string &modelId);

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
  // Test hook: feed a parsed getUsageLimits JSON body and capture tier/quota
  // assignment without hitting the network.
  static bool applyUsageLimitsResponseForTest(
      firmius::shared::OAuthAccount &acc, const std::string &json);

  struct StreamContext {
    KiroProvider *provider;
    std::function<void(const firmius::shared::StreamEvent &)> *onEvent;
    std::string buffer;
    size_t readOffset = 0;
    std::string activeToolUseId;
    std::string contentBuffer;
    bool inThinking = false;
    bool thinkingExtracted = false;
    std::string activeToolName;
    std::string activeToolArgs;
    bool activeToolFinalized = false;
    // Synthetic-thinking-tool state. The Kiro Q endpoint has no wire-protocol
    // opt-in for reasoning across all models; instead, Kiro registers a
    // synthetic `thinking` tool, every model calls it for hard prompts, and
    // the client converts those calls into reasoning events. We do the same
    // here: when a tool call's name == "thinking" we suppress the
    // ToolCall/ToolCallChunk events and emit ThinkingChunks as the `thought`
    // string streams in. After the original stream ends we synthesize a
    // tool_result and make a continuation request so the model can produce
    // its actual answer.
    bool activeIsThinking = false;
    std::string thinkingEmittedSoFar; ///< Substring of `thought` already emitted as deltas.
    std::vector<std::pair<std::string, std::string>>
        completedThinkingCalls; ///< (toolUseId, full input JSON) for follow-up.
    std::atomic<bool> *abortSignal = nullptr;
    firmius::shared::AgentMetrics metrics;
    bool metricsReceived = false;
    bool doneReceived = false;
    // Resolved model id (after alias resolution) for this stream — used to
    // pick the right SSE dialect (Anthropic vs OpenAI/MiniMax/DeepSeek/GLM/Qwen).
    std::string resolvedModelId;
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

  // Single-attempt outcome used by the rotation loop in stream().
  enum class AttemptOutcome {
    Success,      ///< Stream completed cleanly.
    AuthFailed,   ///< 401/403 from the backend (token rejected).
    RateLimited,  ///< 429 from the backend.
    OtherFailure, ///< Network error, 5xx, or curl init failure.
  };

  // Model data
  struct KiroModel {
    std::string id;
    std::string displayName;
    double creditMultiplier;
    std::uint32_t contextWindow;
    std::uint32_t maxOutput;
    std::vector<std::string> modalities;
    bool supportsThinking = false;
    KiroTier minimumTier = KiroTier::Free; ///< Lowest tier that can call this model.
  };
  static std::vector<KiroModel> getKiroModels();
  static std::string resolveModelId(const std::string &modelId);

  static size_t sseWriteCallback(char *ptr, size_t size, size_t nmemb, void *userdata);

  // Token management
  bool refreshTokenIDC(firmius::shared::OAuthAccount &acc);
  bool refreshTokenDesktop(firmius::shared::OAuthAccount &acc);
  // Refreshes the account's access token if it is within the expiry window
  // and persists the new credentials. Returns false only on a real refresh
  // failure (i.e. the refresh token itself is no longer valid).
  bool ensureFreshToken(firmius::shared::OAuthAccount &acc);

  // Quota / plan
  bool fetchUsageLimits(firmius::shared::OAuthAccount &acc);
  static bool parseUsageLimitsResponse(firmius::shared::OAuthAccount &acc,
                                       const std::string &body);

  // Account selection that respects tier gating for the requested model.
  std::optional<firmius::shared::OAuthAccount> getAvailableAccountForModel(
      const std::string &modelId);
  // Ordered list of indices into accounts_ for round-robin traversal during
  // stream(). Caller must hold accountsMutex_. Tier-known qualifying accounts
  // come first, unknown-tier accounts (which we let the backend gate) come
  // after.
  std::vector<int> collectCandidateAccountIndices(const std::string &modelId);

  // One streaming attempt against a single account. Emits chunks/metrics on
  // success; returns an AttemptOutcome so the caller can rotate to another
  // account on failure. The real error from this attempt (HTTP body, curl
  // message, etc.) is always written to *outError when non-null, so the
  // rotation loop can surface the actual cause from the last attempted
  // account rather than fabricate a generic message. When emitTransientErrors
  // is false the StreamError is captured but not emitted.
  AttemptOutcome streamOnceForAccount(
      const firmius::shared::AgentHistory &history,
      const ProviderOptions &opts,
      firmius::shared::OAuthAccount &acc,
      const std::string &resolvedModel,
      bool emitTransientErrors,
      std::function<void(const firmius::shared::StreamEvent &)> onEvent,
      std::optional<firmius::shared::StreamError> *outError = nullptr);

  // Request building
  std::string buildCodeWhispererRequest(
      const firmius::shared::AgentHistory &history,
      const std::string &modelId,
      const firmius::shared::OAuthAccount &acc,
      const ProviderOptions &opts);
};

} // namespace firmius::provider

#endif

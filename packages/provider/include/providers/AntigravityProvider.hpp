#pragma once

#include "providers/BaseOAuthProvider.hpp"
#include <atomic>
#include <functional>
#include <map>
#include <unordered_map>
#include <rapidjson/document.h>
#include <memory>

namespace firmius::provider {
class GoogleSearchProvider;


class AntigravityProvider : public BaseOAuthProvider {
public:
  AntigravityProvider();
  ~AntigravityProvider() override;

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

  std::optional<OAuthAccount> getAvailableAccount(
      const std::optional<std::string> &modelId = std::nullopt) override;

  struct StreamedToolCallState {
    std::string emittedId;
    std::string lastName;
    std::string lastArgs;
    std::uint32_t index = 0;
    bool finalized = false;
    bool hasIndex = false;
  };

  struct StreamContext {
    AntigravityProvider *provider;
    std::function<void(const StreamEvent &)> *onEvent;
    std::string buffer;
    size_t readOffset = 0;
    std::atomic<bool> *abortSignal;
    std::uint32_t toolCallCounter = 0;
    std::unordered_map<std::string, StreamedToolCallState> streamedToolCalls;
  };

  void processSSELine(const std::string &line, StreamContext &ctx);

  // Queries Antigravity to get quota definitions for the given account
  bool fetchAndStoreQuotas(OAuthAccount &acc);


  // Fetches the managed project ID from Antigravity
  std::string fetchManagedProject(OAuthAccount &acc);
  // Resolves a usable project ID for streaming requests.
  std::string resolveProjectIdForAccount(OAuthAccount &acc, bool forceRefresh);

  static std::map<std::string, ModelInfo> getStaticModels();


  static size_t sseWriteCallback(char *ptr, size_t size, size_t nmemb, void *userdata);

private:
  std::unique_ptr<GoogleSearchProvider> searchProvider_;
};

} // namespace firmius::provider

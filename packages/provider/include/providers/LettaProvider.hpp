#pragma once

#include "providers/BaseOAuthProvider.hpp"
#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <unordered_map>

namespace firmius::provider {

class LettaProvider : public BaseOAuthProvider {
public:
  LettaProvider();
  ~LettaProvider() override;

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

  // ------------------------------------------------------------------------
  // Letta-specific helpers
  // ------------------------------------------------------------------------

  static std::map<std::string, ModelInfo> getStaticModels();

  struct StreamedToolCallState {
    std::string emittedId;
    std::string lastName;
    std::string lastArgs;
    std::uint32_t index = 0;
    bool hasIndex = false;
  };

  struct StreamContext {
    LettaProvider *provider;
    std::function<void(const StreamEvent &)> *onEvent;
    std::string buffer;
    size_t readOffset = 0;
    std::atomic<bool> *abortSignal;
    std::uint32_t toolCallCounter = 0;
    std::unordered_map<std::string, StreamedToolCallState> streamedToolCalls;
    bool sawTypedEvent = false;
    bool sawStopReason = false;
    bool sawProtocolError = false;
    StopReason stopReason = StopReason::Stop;
  };

  void processSSELine(const std::string &line, StreamContext &ctx);

private:
  bool fetchAndStoreQuotas(OAuthAccount &acc);
  bool ensureAgentId(OAuthAccount &acc, std::string &outErrorMessage);
  bool ensureConversationId(OAuthAccount &acc, std::string &outConversationId,
                            std::string &outErrorMessage);
  int executeStreamRequest(OAuthAccount &acc, const AgentHistory &history,
                           const ProviderOptions &opts,
                           std::function<void(const StreamEvent &)> &onEvent);

  static size_t sseWriteCallback(char *ptr, size_t size, size_t nmemb,
                                 void *userdata);
};

} // namespace firmius::provider

#pragma once

#include "providers/BaseOAuthProvider.hpp"
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace firmius::provider {

class CodexProvider : public BaseOAuthProvider {
public:
  CodexProvider();
  ~CodexProvider() override = default;
  std::vector<ModelInfo> listModels() override;
  ModelInfo getModelInfo(const std::string &modelId) override;
  void stream(const AgentHistory &history, const ProviderOptions &opts,
              std::function<void(const StreamEvent &)> onEvent) override;
  void generateSummary(const std::string &modelId, const AgentHistory &history,
                       const std::string &compactionPrompt,
                       std::function<void(const StreamEvent &)> onEvent,
                       std::atomic<bool> *abortSignal = nullptr) override;
  std::unique_ptr<OAuthWizard> beginConnectionWizard() override;
  bool refreshAccessToken(OAuthAccount &acc) override;
  void refreshQuotas() override;
  std::map<std::string, std::vector<QuotaBucket>> getAllQuotas() const override;
  std::optional<OAuthAccount *> getAvailableAccount(
      const std::optional<std::string> &modelId = std::nullopt) override;

private:
  struct ToolCallState {
    std::string itemId;
    std::string callId;
    std::string name;
  };

  struct ToolCallTracker {
    std::map<int, ToolCallState> byIndex;
    std::map<std::string, int> indexByItemId;
  };

  static std::map<std::string, ModelInfo> getStaticModels();
  static std::string normalizeModelId(const std::string &modelId);
  static std::string resolveEffort(const std::string &modelId);
  static bool supportsNoneEffort(const std::string &modelId);
  static bool supportsXhighEffort(const std::string &modelId);
  static bool isCodexMini(const std::string &modelId);
  static std::string getQuotaKey(const std::string &modelId);
  static size_t sseWriteCallback(char *ptr, size_t size, size_t nmemb,
                                 void *userdata);
  void processSseLine(const std::string &line,
                      std::function<void(const StreamEvent &)> &onEvent,
                      AgentMetrics &metrics, bool &metricsReceived,
                      bool &doneReceived, ToolCallTracker &tracker);
  void fetchAndStoreQuotas(OAuthAccount &acc);
};

} // namespace firmius::provider

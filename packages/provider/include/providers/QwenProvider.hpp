#pragma once

#include "providers/BaseOAuthProvider.hpp"
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace firmius::provider {

/**
 * @brief Qwen Code OAuth provider (portal.qwen.ai).
 * 
 * Implements OAuth 2.0 Device Flow (RFC 8628) for authentication.
 * Uses PKCE (RFC 7636) for secure token exchange.
 * 
 * Endpoints:
 *   - Device Code: https://chat.qwen.ai/api/v1/oauth2/device/code
 *   - Token:       https://chat.qwen.ai/api/v1/oauth2/token
 *   - API Base:    https://portal.qwen.ai/v1
 * 
 * Models (v1.5.0):
 *   - qwen3.5-plus: Latest hybrid & vision model (1M context, reasoning)
 *   - qwen3-coder-plus: Qwen 3.0 coding model (1M context)
 *   - qwen3-coder-flash: Fast coding model (1M context)
 *   - coder-model: Auto-routed alias (maps to Qwen 3.5 Plus)
 *   - vision-model: Vision-language model (128K context)
 */
class QwenProvider : public BaseOAuthProvider {
public:
  enum class StreamAttemptKind {
    Success,
    QuotaLimited,
    RetryableTransient,
    NonRetryableRequest,
    AuthError,
    PartialResponseError,
  };

  struct StreamAttemptResult {
    StreamAttemptKind kind = StreamAttemptKind::RetryableTransient;
    int httpStatus = 0;
    int retryAfterMs = 0;
    std::string errorMessage;

    bool succeeded() const { return kind == StreamAttemptKind::Success; }
  };

  struct RequestPayloadBuildResult {
    std::string body;
    std::vector<std::string> warnings;
    size_t droppedToolCalls = 0;
    size_t droppedToolResults = 0;
  };

  QwenProvider();
  ~QwenProvider() override = default;

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

  // Parses SSE chunks arriving from the CURL callback
  void processSSELine(const std::string &line,
                      std::function<void(const StreamEvent &)> &onEvent);

  static StreamAttemptResult
  classifyStreamFailure(int httpStatus, const std::string &responseBody,
                        const std::map<std::string, std::string> &headers);
  static bool hasAlternativeAccount(
      const std::vector<OAuthAccount> &accounts,
      const std::string &currentAccountIdentifier);
  static std::string
  composeNoAlternateAccountError(const std::string &underlyingCause);
  static std::string formatErrorMessage(const std::string &modelId,
                                        int httpStatus,
                                        const std::string &responseBody,
                                        const std::string &summary);
  static bool isMeaningfulStreamEvent(const StreamEvent &event);
  static void mergeAccumulatedToolCallChunk(
      std::vector<firmius::shared::ToolCallChunk> &accumulated,
      const firmius::shared::ToolCallChunk &incoming);
  static std::optional<std::string> validateCompletedToolCallBatch(
      const std::vector<firmius::shared::ToolCallChunk> &calls);
  static RequestPayloadBuildResult
  buildRequestPayload(const AgentHistory &history, const ProviderOptions &opts);

protected:
  // OAuth configuration constants (public for use in wizard)
  static const std::string QWEN_OAUTH_CLIENT_ID;
  static const std::string QWEN_OAUTH_SCOPE;
  static const std::string QWEN_OAUTH_GRANT_TYPE;
  static const std::string QWEN_OAUTH_DEVICE_CODE_ENDPOINT;
  static const std::string QWEN_OAUTH_TOKEN_ENDPOINT;
  static const std::string QWEN_API_BASE_URL;
  static const std::string QWEN_CHAT_ENDPOINT;
  static const std::string QWEN_MODELS_ENDPOINT;

  // Helper for wizard to access constants
  friend class QwenOAuthWizard;

private:
  static std::map<std::string, ModelInfo> getStaticModels();

  // Sends the OpenAI-compatible chat completions request
  StreamAttemptResult
  executeStreamRequest(OAuthAccount &acc, const AgentHistory &history,
                       const ProviderOptions &opts,
                       std::function<void(const StreamEvent &)> &onEvent);

  static size_t sseWriteCallback(char *ptr, size_t size, size_t nmemb,
                                 void *userdata);
};

} // namespace firmius::provider

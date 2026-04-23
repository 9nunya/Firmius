#ifndef FIRMIUS_PROVIDER_BASE_ANTHROPIC_PROVIDER_HPP
#define FIRMIUS_PROVIDER_BASE_ANTHROPIC_PROVIDER_HPP

#include "IProvider.hpp"
#include "providers/BackoffConstants.hpp"
#include "providers/BaseAPIKeyProvider.hpp"
#include <string>
#include <map>
#include <vector>
#include <chrono>
#include <functional>
#include <atomic>

namespace firmius::provider {

/**
 * @brief Structure to capture response headers during CURL requests.
 */
struct AnthropicHeaderCaptureContext {
    long httpStatus = 0;                            ///< HTTP status code from response.
    int retryAfterMs = 0;                           ///< Retry-After header value in milliseconds.
    bool headersParsed = false;                     ///< Whether headers have been parsed.
};

// Forward declaration - defined after BaseAnthropicProvider class
struct AnthropicStreamContext;

/**
 * @brief Base provider class for Anthropic-compatible APIs.
 * 
 * Handles Anthropic's SSE format with content_block_delta events,
 * interleaved thinking, and tool calls.
 * 
 * Subclasses should override:
 * - getBaseUrl() to return the API base URL
 * - getHeaders() to add provider-specific headers
 * - listModels() to return available models
 */
class BaseAnthropicProvider : public BaseAPIKeyProvider {
public:
    BaseAnthropicProvider(std::string id, const std::string& baseUrl, const std::string& apiKey);

    // getId() and getProviderType() inherited from BaseAPIKeyProvider

    void stream(const AgentHistory& history, const ProviderOptions& opts,
                std::function<void(const StreamEvent&)> onEvent) override;

    std::vector<ModelInfo> listModels() override;

    /**
     * @brief Process a single SSE line from the Anthropic API.
     * @param line The SSE line (including "data: " prefix).
     * @param onEvent Callback to emit events.
     * @param ctx Stream context for stateful parsing.
     */
    void processSSELine(const std::string& line, 
                        std::function<void(const StreamEvent&)>& onEvent,
                        AnthropicStreamContext& ctx);

    void generateSummary(const std::string &modelId, const AgentHistory &history,
                         const std::string &compactionPrompt,
                         std::function<void(const StreamEvent &)> onEvent,
                         std::atomic<bool> *abortSignal = nullptr) override;

    /**
     * @brief Begin the API key connection wizard.
     * @return Wizard instance for API key input.
     */
    std::unique_ptr<APIKeyWizard> beginConnectionWizard() override;

    static std::string formatErrorMessage(const std::string& providerId,
                                          const std::string& modelId,
                                          int httpStatus,
                                          const std::string& responseBody,
                                          const std::string& prefix);

protected:
    std::string baseUrl;

    /**
     * @brief Get the base URL for API requests.
     * Can be overridden by subclasses.
     */
    virtual std::string getBaseUrl() const { return baseUrl; }

    /**
     * @brief Get headers for API requests.
     * Subclasses can override to add custom headers.
     */
    virtual std::map<std::string, std::string> getHeaders();

    /**
     * @brief Prepare the request body for the Anthropic API.
     * Subclasses can override to customize the payload.
     */
    virtual std::string prepareRequestBody(const AgentHistory& history, const ProviderOptions& opts);

    /**
     * @brief Get the Anthropic beta header value if needed.
     * Override to enable beta features like interleaved thinking.
     */
    virtual std::string getAnthropicBetaHeader() const { return ""; }

    /**
     * @brief Whether this provider supports prompt caching.
     * Override to return true for providers that support it.
     */
    virtual bool supportsPromptCaching() const { return false; }

    /**
     * @brief Calculates estimated cost based on token usage and model pricing.
     */
    void calculateCost(AgentMetrics& metrics, const ModelInfo& model) const;

    /**
     * @brief Returns the ModelInfo for a given model ID.
     * Default implementation does a linear scan of listModels(). Override for caching.
     */
    ModelInfo getModelInfo(const std::string& modelId) override;

    /**
     * @brief Whether to include usage in the stream response.
     */
    virtual bool includeUsage() const { return true; }

private:
    /**
     * @brief Returns current time in milliseconds since epoch.
     */
    static std::uint64_t nowMs();

    /**
     * @brief CURL header callback to capture response headers.
     */
    static size_t headerCallback(char* ptr, size_t size, size_t nmemb, void* userdata);

    /**
     * @brief CURL write callback for SSE data.
     */
    static size_t sseWriteCallback(char* ptr, size_t size, size_t nmemb, void* userdata);

    /**
     * @brief Checks if an HTTP status code represents a retriable error.
     */
    bool isRetriableStatus(int httpStatus) const;

    /**
     * @brief Checks if an HTTP status code represents a non-retriable error.
     */
    bool isNonRetriableStatus(int httpStatus) const;

    /**
     * @brief Calculates the delay for the next retry attempt with jitter.
     */
    int calculateRetryDelay(int attempt, int headerDelayMs) const;

    /**
     * @brief Parses retry-related headers and returns delay in milliseconds.
     */
    int parseRetryHeaders(const std::string& headerLine) const;

    std::vector<ModelInfo> cachedModels;
    bool modelsCached = false;
};

/**
 * @brief Stream context for Anthropic SSE parsing.
 */
struct AnthropicStreamContext {
    BaseAnthropicProvider* provider = nullptr;
    std::function<void(const StreamEvent&)>* onEvent = nullptr;
    std::string buffer;
    size_t readOffset = 0;
    std::atomic<bool>* abortSignal = nullptr;

    // State for incremental parsing
    std::string currentContentBlockType;
    int currentContentBlockIndex = -1;
    std::string currentToolCallId;
    std::string currentToolCallName;
    std::string currentToolCallArgs;
    int currentToolCallIndex = -1;
    bool inToolCall = false;
    
    // Usage tracking - capture from message_start, emit at message_delta
    std::uint32_t inputTokens = 0;
    std::uint32_t outputTokens = 0;
    std::uint32_t cacheRead = 0;
    std::uint32_t cacheWrite = 0;
    bool usageCaptured = false;
};

} // namespace firmius::provider

#endif // FIRMIUS_PROVIDER_BASE_ANTHROPIC_PROVIDER_HPP

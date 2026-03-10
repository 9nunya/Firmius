#ifndef FIRMIUS_PROVIDER_BASE_OPENAI_PROVIDER_HPP
#define FIRMIUS_PROVIDER_BASE_OPENAI_PROVIDER_HPP

#include "IProvider.hpp"
#include <string>
#include <map>
#include <vector>
#include <chrono>
#include <functional>

namespace firmius::provider {

/**
 * @brief Constants for retry logic with exponential backoff.
 */
struct RetryConstants {
    static constexpr int BASE_DELAY_MS = 1000;      ///< Base delay for exponential backoff (1 second).
    static constexpr int MAX_DELAY_MS = 30000;      ///< Maximum delay cap (30 seconds).
    static constexpr int MAX_RETRIES = 5;           ///< Maximum number of retry attempts.
    static constexpr double JITTER_MIN = 0.5;       ///< Minimum jitter multiplier.
    static constexpr double JITTER_MAX = 1.0;       ///< Maximum jitter multiplier.
};

/**
 * @brief Structure to capture response headers during CURL requests.
 */
struct HeaderCaptureContext {
    long httpStatus = 0;                            ///< HTTP status code from response.
    int retryAfterMs = 0;                           ///< Retry-After header value in milliseconds.
    bool headersParsed = false;                     ///< Whether headers have been parsed.
};

class BaseOpenAIProvider : public IProvider {
public:
    BaseOpenAIProvider(std::string id, const std::string& baseUrl, const std::string& apiKey);
    
    std::string getId() const override { return providerId; }
    ProviderType getProviderType() const override;

    void stream(const AgentHistory& history, const ProviderOptions& opts, 
                std::function<void(const StreamEvent&)> onEvent) override;
    
    std::vector<ModelInfo> listModels() override;

    void processSSELine(const std::string& line, std::function<void(const StreamEvent&)>& onEvent);

    void generateSummary(const std::string &modelId, const AgentHistory &history,
                         const std::string &compactionPrompt,
                         std::function<void(const StreamEvent &)> onEvent,
                         std::atomic<bool> *abortSignal = nullptr) override;

protected:
    std::string providerId;
    std::string baseUrl;
    std::string apiKey;

    virtual std::map<std::string, std::string> getHeaders();
    virtual std::string prepareRequestBody(const AgentHistory& history, const ProviderOptions& opts);
    virtual std::string getReasoningFieldName() const;

    /**
     * @brief Calculates estimated cost based on token usage and model pricing.
     */
    void calculateCost(AgentMetrics& metrics, const ModelInfo& model) const;

    /**
     * @brief Whether this provider supports stream_options.include_usage.
     * Override to return false for providers that don't support it.
     */
    virtual bool supportsStreamUsage() const;

    /**
     * @brief Returns the ModelInfo for a given model ID (for cost calculation).
     * Default implementation does a linear scan of listModels(). Override for caching.
     */
    virtual ModelInfo getModelInfo(const std::string& modelId);

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

}

#endif

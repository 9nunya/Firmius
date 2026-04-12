#ifndef FIRMIUS_SHARED_METRICS_HPP
#define FIRMIUS_SHARED_METRICS_HPP

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

/**
 * @brief Telemetry and performance tracking types.
 */
namespace firmius::shared {

/**
 * @brief Tracks token usage for LLM requests.
 */
struct TokenMetrics {
  std::uint32_t prompt = 0;     ///< Number of input tokens (billed) in the last request or cumulative.
  std::uint32_t completion = 0; ///< Number of output tokens in the last request or cumulative.
  std::uint32_t reasoning = 0;  ///< Number of tokens used for internal reasoning/thinking.
  std::uint32_t cacheRead = 0;   ///< Cached input tokens (prompt cache hits).
  std::uint32_t cacheWrite = 0;  ///< Cache creation tokens (prompt cache misses written).
  std::uint32_t contextSize = 0; ///< Latest context window size (raw prompt tokens).
  std::uint32_t cumulativePrompt = 0; ///< Cumulative billed input tokens.
  std::uint32_t total = 0;      ///< Cumulative total tokens (billed prompt + completion).

  bool operator==(const TokenMetrics& other) const = default;
};

/**
 * @brief One labeled contribution to the context sent to the model.
 */
struct ContextBucketMetrics {
  std::string label;              ///< Stable bucket label.
  std::uint32_t estimatedTokens = 0; ///< Pre-send estimate for this bucket.
  std::uint32_t actualTokens = 0;    ///< Reconciled prompt-token share.

  bool operator==(const ContextBucketMetrics& other) const = default;
};

/**
 * @brief Latest context accounting snapshot for a model request.
 */
struct ContextWindowMetrics {
  std::uint32_t sentTokens = 0;        ///< Estimated total prompt tokens before provider usage arrives.
  std::uint32_t rawPromptTokens = 0;   ///< Provider-reported prompt/context total before cache subtraction.
  std::uint32_t billedPromptTokens = 0; ///< Provider-billed prompt tokens after cache subtraction.
  std::uint32_t reserveTokens = 0;     ///< Unallocated remainder after reconciliation.
  std::vector<ContextBucketMetrics> buckets;

  [[nodiscard]] bool empty() const {
    return sentTokens == 0 && rawPromptTokens == 0 && billedPromptTokens == 0 &&
           reserveTokens == 0 && buckets.empty();
  }

  bool operator==(const ContextWindowMetrics& other) const = default;
};

/**
 * @brief Tracks latency and execution timing.
 */
struct TimingMetrics {
  std::uint64_t startMs = 0;          ///< Request start timestamp.
  std::uint64_t firstTokenMs = 0;     ///< Time to first token in ms.
  std::uint64_t endMs = 0;            ///< Request completion timestamp.
  std::uint64_t toolExecutionMs = 0;  ///< Total time spent executing tools in ms.

  bool operator==(const TimingMetrics& other) const = default;
};

/**
 * @brief Aggregated metrics for an agent turn or task.
 */
struct AgentMetrics {
  TokenMetrics tokens;      ///< Token consumption details.
  TimingMetrics timing;     ///< Latency details.
  double estimatedCostUsd = 0.0; ///< Calculated cost of the request.
  ContextWindowMetrics context;   ///< Latest context-bucketing snapshot.

  /**
   * @brief Accumulates metrics from another instance.
   */
  AgentMetrics& operator+=(const AgentMetrics& other) {
    tokens.prompt += other.tokens.prompt; // Cumulative billed
    tokens.completion += other.tokens.completion;
    tokens.reasoning += other.tokens.reasoning;
    tokens.cacheRead += other.tokens.cacheRead;
    tokens.cacheWrite += other.tokens.cacheWrite;
    tokens.contextSize = other.tokens.contextSize; // Latch current window size
    tokens.cumulativePrompt += other.tokens.prompt;
    tokens.total += other.tokens.total; // Accumulate from other.total
    // Timing: min/max semantics for timestamps, additive for durations
    if (other.timing.startMs != 0) {
        timing.startMs = (timing.startMs == 0)
            ? other.timing.startMs
            : std::min(timing.startMs, other.timing.startMs);
    }
    if (other.timing.firstTokenMs != 0) {
        timing.firstTokenMs = (timing.firstTokenMs == 0)
            ? other.timing.firstTokenMs
            : std::min(timing.firstTokenMs, other.timing.firstTokenMs);
    }
    timing.endMs = std::max(timing.endMs, other.timing.endMs);
    timing.toolExecutionMs += other.timing.toolExecutionMs;
    estimatedCostUsd += other.estimatedCostUsd;
    if (!other.context.empty()) {
      context = other.context;
    }
    return *this;
  }

  bool operator==(const AgentMetrics& other) const = default;
};

}

#endif

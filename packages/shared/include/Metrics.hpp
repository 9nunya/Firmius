#ifndef FIRMIUS_SHARED_METRICS_HPP
#define FIRMIUS_SHARED_METRICS_HPP

#include <cstdint>
#include <algorithm>

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
    return *this;
  }

  bool operator==(const AgentMetrics& other) const = default;
};

}

#endif

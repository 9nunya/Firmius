#ifndef FIRMIUS_SHARED_METRICS_HPP
#define FIRMIUS_SHARED_METRICS_HPP

#include <cstdint>

/**
 * @brief Telemetry and performance tracking types.
 */
namespace firmius::shared {

/**
 * @brief Tracks token usage for LLM requests.
 */
struct TokenMetrics {
  std::uint32_t prompt = 0;     ///< Number of input tokens.
  std::uint32_t completion = 0; ///< Number of output tokens.
  std::uint32_t reasoning = 0;  ///< Number of tokens used for internal reasoning/thinking.
  std::uint32_t total = 0;      ///< Total tokens consumed.

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
    tokens.prompt += other.tokens.prompt;
    tokens.completion += other.tokens.completion;
    tokens.reasoning += other.tokens.reasoning;
    tokens.total += other.tokens.total;
    timing.startMs += other.timing.startMs;
    timing.firstTokenMs += other.timing.firstTokenMs;
    timing.endMs += other.timing.endMs;
    timing.toolExecutionMs += other.timing.toolExecutionMs;
    estimatedCostUsd += other.estimatedCostUsd;
    return *this;
  }

  bool operator==(const AgentMetrics& other) const = default;
};

}

#endif

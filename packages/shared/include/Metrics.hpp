#ifndef FIRMIUS_SHARED_METRICS_HPP
#define FIRMIUS_SHARED_METRICS_HPP

#include "Enums.hpp"

#include <algorithm>
#include <cstdint>
#include <map>
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
 * @brief Tracks quota usage for a single provider request.
 */
struct QuotaMetrics {
  std::string providerId;       ///< Provider identifier (e.g., "codex", "antigravity").
  std::string accountLocator;  ///< Account identifier/email used for this request.
  std::string modelId;         ///< Model identifier used.
  
  std::vector<QuotaBucket> quotaBefore;  ///< Quota snapshot before request.
  std::vector<QuotaBucket> quotaAfter;   ///< Quota snapshot after request.
  std::map<std::string, float> quotaDiff; ///< Bucket name -> fraction consumed.
  
  // Provider-specific primary bucket tracking
  std::string primaryBucketName;      ///< Name of the primary bucket for this model (e.g., "5h", "claude-3-5").
  float primaryBucketDiff = 0.0f;      ///< Fraction consumed from primary bucket.
  float primaryBucketRemaining = 0.0f; ///< Remaining fraction in primary bucket after request.
  
  bool rateLimited = false;     ///< Whether request hit rate limit.
  int64_t backoffUntil = 0;     ///< Epoch seconds when backoff expires (if rate limited).
  int retryAttempt = 0;         ///< Which retry attempt this was (0 = first attempt).
  
  /**
   * @brief Calculates quota diffs from before/after snapshots.
   * @param modelId Optional model ID to determine primary bucket (provider-specific).
   */
  void calculateDiffs(const std::string& primaryBucketHint = "") {
    quotaDiff.clear();
    for (const auto& after : quotaAfter) {
      for (const auto& before : quotaBefore) {
        if (before.name == after.name) {
          float diff = before.remainingFraction - after.remainingFraction;
          if (diff > 0.0f) {
            quotaDiff[after.name] = diff;
          }
          // Track primary bucket if hint matches or it's the first bucket
          if (!primaryBucketHint.empty() && after.name == primaryBucketHint) {
            primaryBucketName = after.name;
            primaryBucketDiff = diff;
            primaryBucketRemaining = after.remainingFraction;
          }
          break;
        }
      }
    }
    // If no primary bucket found but we have a hint, try partial match
    if (primaryBucketName.empty() && !primaryBucketHint.empty()) {
      for (const auto& after : quotaAfter) {
        if (after.name.find(primaryBucketHint) != std::string::npos ||
            primaryBucketHint.find(after.name) != std::string::npos) {
          for (const auto& before : quotaBefore) {
            if (before.name == after.name) {
              primaryBucketName = after.name;
              primaryBucketDiff = before.remainingFraction - after.remainingFraction;
              primaryBucketRemaining = after.remainingFraction;
              break;
            }
          }
          if (!primaryBucketName.empty()) break;
        }
      }
    }
    // Fallback: use first bucket with consumption, or just first bucket
    if (primaryBucketName.empty()) {
      for (const auto& [name, diff] : quotaDiff) {
        if (diff > 0.0f) {
          primaryBucketName = name;
          primaryBucketDiff = diff;
          for (const auto& after : quotaAfter) {
            if (after.name == name) {
              primaryBucketRemaining = after.remainingFraction;
              break;
            }
          }
          break;
        }
      }
    }
  }
  
  bool operator==(const QuotaMetrics& other) const = default;
};

/**
 * @brief Working-memory (rolling memory v2) telemetry for an agent turn or
 * thread.
 *
 * All numbers are reported in tokens unless noted. Counters are cumulative
 * across the thread's lifetime when aggregated; per-turn snapshots are
 * additive into the cumulative aggregate via operator+= below.
 */
struct MemoryMetrics {
  // Working set vs raw history accounting (the v1-killer story).
  std::uint32_t rawHistoryTokens = 0;   ///< Tokens in the full journal-loaded history.
  std::uint32_t workingSetTokens = 0;   ///< Tokens that ended up in the request after assembly.
  std::uint32_t pinnedTurnCount = 0;    ///< Turns hard-pinned by policy this turn.
  std::uint32_t evictedTurnCount = 0;   ///< Turns dropped from working set this turn.
  std::uint32_t recalledTurnCount = 0;  ///< Turns re-pinned by relevance fill this turn.
  std::uint32_t deflatedPartCount = 0;  ///< MessageParts replaced by deflation stub this turn.

  // Token spend / save accounting (per-turn, additive).
  std::uint32_t tokensSavedByDeflation = 0;
  std::uint32_t tokensSavedByEviction = 0;
  std::uint32_t tokensSpentOnSummaries = 0;     ///< Summary stub tokens injected.
  std::uint32_t tokensSpentOnEmbeddings = 0;    ///< Query-embedding text token cost.
  std::uint32_t tokensSpentOnOverlays = 0;      ///< Overlay system-message tokens injected.

  // Focus metrics — the v1 failure detectors.
  std::uint32_t userPromptsRetained = 0;        ///< User turns present in working set this request.
  std::uint32_t userPromptsTotal = 0;           ///< User turns ever appended.
  std::uint32_t imagePartsRetained = 0;         ///< Image parts present in working set this request.
  std::uint32_t imagePartsTotal = 0;            ///< Image parts ever appended.
  std::uint32_t redundantReadCount = 0;         ///< read_file calls on already-read paths.
  std::uint32_t redundantToolSignatureCount = 0; ///< Repeats of (toolName,args) signatures.

  // Hot-path latency contribution (per turn, additive when aggregated).
  std::uint64_t hotPathLatencyMicros = 0;       ///< Time the working-memory layer spent on the agent thread.

  /// True if the working-memory layer crossed the buffer threshold during this
  /// turn (i.e. did anything beyond pass-through).
  bool aboveBufferThreshold = false;
  /// True if it crossed the target threshold (relevance fill + deflation enabled).
  bool aboveTargetThreshold = false;
  /// True if it crossed the emergency threshold (synchronous deflation forced).
  bool aboveEmergencyThreshold = false;

  bool operator==(const MemoryMetrics& other) const = default;
};

/**
 * @brief Aggregated metrics for an agent turn or task.
 */
struct AgentMetrics {
  TokenMetrics tokens;      ///< Token consumption details.
  TimingMetrics timing;     ///< Latency details.
  double estimatedCostUsd = 0.0; ///< Calculated cost of the request.
  ContextWindowMetrics context;   ///< Latest context-bucketing snapshot.
  QuotaMetrics quota;       ///< Quota usage details for this request.
  MemoryMetrics memory;     ///< Working-memory telemetry.

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
    // Quota: take the latest quota snapshot (not additive)
    if (!other.quota.providerId.empty()) {
      quota = other.quota;
    }
    // Memory: per-request snapshot fields are latched (latest value wins);
    // counters are additive across turns.
    memory.rawHistoryTokens = other.memory.rawHistoryTokens;
    memory.workingSetTokens = other.memory.workingSetTokens;
    memory.pinnedTurnCount = other.memory.pinnedTurnCount;
    memory.evictedTurnCount = other.memory.evictedTurnCount;
    memory.recalledTurnCount = other.memory.recalledTurnCount;
    memory.deflatedPartCount += other.memory.deflatedPartCount;
    memory.tokensSavedByDeflation += other.memory.tokensSavedByDeflation;
    memory.tokensSavedByEviction += other.memory.tokensSavedByEviction;
    memory.tokensSpentOnSummaries += other.memory.tokensSpentOnSummaries;
    memory.tokensSpentOnEmbeddings += other.memory.tokensSpentOnEmbeddings;
    memory.tokensSpentOnOverlays += other.memory.tokensSpentOnOverlays;
    memory.userPromptsRetained = other.memory.userPromptsRetained;
    memory.userPromptsTotal = std::max(memory.userPromptsTotal, other.memory.userPromptsTotal);
    memory.imagePartsRetained = other.memory.imagePartsRetained;
    memory.imagePartsTotal = std::max(memory.imagePartsTotal, other.memory.imagePartsTotal);
    memory.redundantReadCount += other.memory.redundantReadCount;
    memory.redundantToolSignatureCount += other.memory.redundantToolSignatureCount;
    memory.hotPathLatencyMicros += other.memory.hotPathLatencyMicros;
    memory.aboveBufferThreshold = other.memory.aboveBufferThreshold;
    memory.aboveTargetThreshold = other.memory.aboveTargetThreshold;
    memory.aboveEmergencyThreshold = other.memory.aboveEmergencyThreshold;
    return *this;
  }

  bool operator==(const AgentMetrics& other) const = default;
};

}

#endif

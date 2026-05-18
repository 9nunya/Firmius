#ifndef FIRMIUS_CORE_WORKING_MEMORY_HPP
#define FIRMIUS_CORE_WORKING_MEMORY_HPP

#include "Context.hpp"
#include "ITokenizer.hpp"
#include "agents/working_memory/Deflator.hpp"
#include "agents/working_memory/PinPolicy.hpp"

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace firmius::core::working_memory {

/**
 * @brief Resolved threshold values for the current actor model.
 *
 * Computed once per request. All token values are derived from the
 * configured ratios times the actor model's context window.
 */
struct ResolvedThresholds {
  std::uint32_t contextWindow = 0;
  std::uint32_t bufferTokens = 0;
  std::uint32_t targetTokens = 0;
  std::uint32_t emergencyTokens = 0;
  std::uint32_t recencyTailTokens = 0;
};

/**
 * @brief Per-request audit/debug report produced by the assembler.
 *
 * Mirrored into AgentMetrics::memory by the caller; also available for
 * direct surfacing in audits or the daemon protocol.
 */
struct WorkingMemoryReport {
  ResolvedThresholds thresholds;
  std::uint32_t rawHistoryTokens = 0;
  std::uint32_t workingSetTokens = 0;
  std::uint32_t hardPinTokens = 0;
  std::uint32_t softPinTokens = 0;
  std::uint32_t evictableTokens = 0;
  std::uint32_t pinnedTurnCount = 0;
  std::uint32_t evictedTurnCount = 0;
  std::uint32_t recalledTurnCount = 0;
  std::uint32_t deflatedPartCount = 0;
  std::uint32_t tokensSavedByDeflation = 0;
  std::uint32_t tokensSavedByEviction = 0;
  std::uint32_t tokensSpentOnSummaries = 0;
  std::uint32_t tokensSpentOnEmbeddings = 0;
  std::uint32_t tokensSpentOnOverlays = 0;
  std::uint32_t userPromptsRetained = 0;
  std::uint32_t userPromptsTotal = 0;
  std::uint32_t imagePartsRetained = 0;
  std::uint32_t imagePartsTotal = 0;
  std::uint64_t hotPathLatencyMicros = 0;
  bool aboveBufferThreshold = false;
  bool aboveTargetThreshold = false;
  bool aboveEmergencyThreshold = false;
  std::vector<std::string> evictedTurnIds;
  std::vector<std::string> recalledTurnIds;
};

/**
 * @brief Inputs required to assemble the working set, beyond the history
 * and context themselves.
 *
 * Decoupled from any singleton so tests / audits can drive the assembler
 * directly with a fake tokenizer, fake embedding source, etc.
 */
struct WorkingMemoryInputs {
  /// Tokenizer used for all token estimation.
  const shared::ITokenizer* tokenizer = nullptr;
  /// Optional retrieval source: given a query string and topK, return
  /// turn IDs from the evicted region that should be recalled. Empty
  /// vector if no embeddings are available. The function may be called
  /// from the agent thread, so it should be fast (<50ms).
  std::function<std::vector<std::string>(const std::string& query, std::size_t topK)>
      relevanceQuery;
  /// Optional active-state references resolver: returns turn IDs that
  /// the agent's active todo / artifacts / edit batches reference.
  /// Called once per assembly. Can be empty when no harness state is
  /// available (e.g. detached audit runs).
  std::function<std::vector<std::string>()> activeStateReferences;
  /// Actor model context window in tokens. If zero, the assembler uses
  /// 128k as a default but reports it in the thresholds.
  std::uint32_t actorContextWindow = 0;
  /// Override for "now" used in deflation horizons (turn age computation).
  /// When zero, falls back to history.turns.size() - 1.
  std::optional<std::size_t> overrideNewestTurnIndex;
  /// Synchronous summarizer to invoke when the layer must deflate parts
  /// on the hot path (emergency threshold). Can be null.
  SummarizerFn synchronousSummarizer;
  /// Working-memory archive used for deflation persistence. Required when
  /// deflation is enabled at the current threshold; can be null when the
  /// caller knows assembly will only pin/evict and never deflate.
  DeflationArchive* archive = nullptr;

  /// Optional asynchronous upgrade hook. When non-null, deflation runs
  /// with deterministic stubs synchronously, and then for each deflated
  /// part this callback is invoked so the caller can enqueue a
  /// background job that upgrades the stub into an LLM summary on a
  /// separate thread. This is the production path: hot-path latency
  /// stays microseconds, summaries appear on subsequent turns.
  using AsyncUpgradeFn = std::function<void(
      const std::string& turnId, std::size_t messageIndex,
      std::size_t partIndex, const std::string& toolName,
      const std::string& toolArgs, const std::string& body,
      const std::string& archiveId)>;
  AsyncUpgradeFn asyncUpgrade;
};

/**
 * @brief Assemble the working set for one request.
 *
 * Pipeline:
 *   1. Resolve thresholds from config + actor context window.
 *   2. Compute pin classification (HardPin / SoftPin / Evictable).
 *   3. Build the working-set turn list:
 *        a. Always include all HardPin turns.
 *        b. Always include all SoftPin turns (they preserve tool pairing).
 *        c. If above target threshold, run relevanceQuery to recall some
 *           Evictable turns; include them.
 *        d. Below buffer threshold, include all Evictable turns too —
 *           pure pass-through.
 *        e. Between buffer and target, include all Evictable turns until
 *           the running token count would exceed `targetTokens`.
 *        f. Above emergency, prefer fewer Evictable turns (only the
 *           recalled set) and run synchronous deflation on remaining
 *           SoftPin parts.
 *   4. If above target, run selectDeflationCandidates + deflateCandidates
 *      against SoftPin turns (HardPin turns are never deflated).
 *
 * Returns the assembled history and writes the audit report into `report`.
 */
shared::AgentHistory assembleWorkingSet(const shared::AgentContext& context,
                                        const shared::AgentHistory& history,
                                        const WorkingMemoryInputs& inputs,
                                        WorkingMemoryReport& report);

/**
 * @brief Compute thresholds from config + actor context window. Exposed for
 * tests / audits and for read-only use by ContextLane.
 */
ResolvedThresholds resolveThresholds(const shared::WorkingMemoryConfig& config,
                                     std::uint32_t actorContextWindow);

/**
 * @brief Convenience: count the user-prompt turns and image parts in a
 * given history. Used by the assembler to compute retention metrics, but
 * also useful for tests + audits that want the raw counts.
 */
struct FocusCounts {
  std::uint32_t userPromptsTotal = 0;
  std::uint32_t imagePartsTotal = 0;
};
FocusCounts countFocusElements(const shared::AgentHistory& history);

/**
 * @brief Build a hard-pin mask aligned with `history.turns`.
 *
 * Index `i` is true iff decisions[i].kind == HardPin. Useful for the
 * Deflator's selectDeflationCandidates input.
 */
std::vector<bool> buildHardPinMask(const PinClassification& classification);

} // namespace firmius::core::working_memory

#endif

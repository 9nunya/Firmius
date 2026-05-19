#ifndef FIRMIUS_AUDITS_WORKINGMEMORYAUDIT_HPP
#define FIRMIUS_AUDITS_WORKINGMEMORYAUDIT_HPP

#include "IAudit.hpp"

namespace firmius::audits {

/**
 * @brief Synthetic-workload audit for the rolling memory v2 layer.
 *
 * Generates a deterministic 200-turn transcript with a realistic mix of
 * user prompts, assistant text, tool-call/tool-result pairs (mostly grep
 * and read), and one image. Then runs WorkingMemory::assembleWorkingSet
 * across a sweep of context-window sizes (effective occupancy ratios)
 * and reports the focus metrics, savings/spend, and hot-path latency.
 *
 * Metrics measured per scenario:
 *   - User prompt retention (must be 100% always)
 *   - Image part retention (must be 100% always)
 *   - Re-read rate (synthesized via repeated read calls on same paths)
 *   - Working set tokens vs raw history tokens
 *   - Tokens saved by deflation, by eviction
 *   - Tokens spent on summaries, embeddings, overlays
 *   - Hot path latency (microseconds)
 *   - Pin / soft-pin / evictable distribution
 */
class WorkingMemoryAudit final : public shared::IAudit {
public:
  std::string getId() const override;
  std::string getDescription() const override;
  shared::AuditResult run(const std::vector<std::string> &args) override;
};

} // namespace firmius::audits

#endif

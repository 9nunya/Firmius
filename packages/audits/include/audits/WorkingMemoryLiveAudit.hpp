#ifndef FIRMIUS_AUDITS_WORKING_MEMORY_LIVE_AUDIT_HPP
#define FIRMIUS_AUDITS_WORKING_MEMORY_LIVE_AUDIT_HPP

#include "IAudit.hpp"

namespace firmius::audits {

/**
 * @brief End-to-end stress test of rolling memory v2 against a real model.
 *
 * Drives a real agent through a scripted scenario designed to exercise
 * every working-memory invariant the layer claims to uphold:
 *
 *   1. Saturate phase: seed a workspace with files containing distinct
 *      MAGIC_PHRASE_<X> markers, ask the agent to find them, let it run
 *      grep + read tool calls until it has accumulated meaningful tool
 *      result history.
 *
 *   2. Threshold phase: mid-conversation, lower the agent's effective
 *      context window via WorkingMemoryConfig::actorContextWindowOverride
 *      so subsequent turns deterministically cross buffer/target/emergency
 *      thresholds in the working-memory layer.
 *
 *   3. Probe phase: inject focus-probe user messages that test specific
 *      retention guarantees:
 *        - "What was the very first thing I asked you to do?"
 *           → user-prompt retention
 *        - "List every magic phrase you found, in order."
 *           → tool-result content recoverability via deflation+archive or
 *             relevance fill or RuntimeOverlay re-read avoidance
 *        - Pin tool round-trip:
 *           "Pin the fact that ALPHA was in fileA.txt."
 *           ... force more turns ...
 *           "What did you pin earlier?"
 *           → agent-driven pinning + pin retention
 *
 *   4. Verdict phase: per-probe PASS/PARTIAL/FAIL with reasoning, plus
 *      aggregate MemoryMetrics (raw vs working-set tokens, redundant-read
 *      counter, hot-path latency, retention).
 *
 * Default model: kilo / stepfun/step-3.5-flash:free (262k context, free).
 * Override via --provider <id> --model <id>.
 * Default actor-window override: 8192 tokens (forces aggressive memory mode
 * after a few tool calls). Override via --window <tokens>; 0 disables.
 */
class WorkingMemoryLiveAudit final : public shared::IAudit {
public:
  std::string getId() const override;
  std::string getDescription() const override;
  shared::AuditResult run(const std::vector<std::string> &args) override;
};

} // namespace firmius::audits

#endif

#ifndef FIRMIUS_CORE_PIN_POLICY_HPP
#define FIRMIUS_CORE_PIN_POLICY_HPP

#include "Context.hpp"
#include "ITokenizer.hpp"

#include <cstdint>
#include <string>
#include <unordered_set>
#include <vector>

namespace firmius::core::working_memory {

/**
 * @brief Per-turn pin classification produced by PinPolicy.
 *
 * HardPin:    must travel in every request, never evictable, never deflatable.
 *             Examples: every Role::User turn, recent-tail turns, turns
 *             holding ImageContent, turns flagged via the pin tool, turns
 *             whose tool results edited a tracked file.
 *
 * SoftPin:    pinned by relationship — typically a tool-call turn whose
 *             tool-result is hard-pinned, or vice versa. The working-set
 *             assembler will keep these to preserve tool-call/result
 *             pairing invariants but they're allowed to be deflated in
 *             place (envelope kept, body stubbed).
 *
 * Evictable:  free to leave the working set when occupancy demands. Still
 *             on disk in the journal; can be re-pinned by relevance fill.
 */
enum class PinKind {
  HardPin,
  SoftPin,
  Evictable,
};

/**
 * @brief Classification result for one turn at one point in time.
 */
struct PinDecision {
  std::string turnId;
  std::size_t turnIndex = 0; ///< Position in the input history.turns vector.
  PinKind kind = PinKind::Evictable;
  std::string reason; ///< Human-readable cause: "user_message", "tail", "image", ...
  std::uint32_t estimatedTokens = 0;
};

/**
 * @brief Aggregate result of classifying an entire history.
 */
struct PinClassification {
  std::vector<PinDecision> decisions; ///< One per input turn, same order.
  std::uint32_t hardPinTokens = 0;
  std::uint32_t softPinTokens = 0;
  std::uint32_t evictableTokens = 0;
  std::uint32_t totalTokens = 0;

  /// Convenience: count of decisions per kind.
  std::uint32_t hardPinCount = 0;
  std::uint32_t softPinCount = 0;
  std::uint32_t evictableCount = 0;
};

/**
 * @brief Inputs to the pin policy beyond the history itself.
 */
struct PinPolicyInputs {
  /// Recency-tail token budget. The trailing turns adding up to this many
  /// tokens (counted backwards from the newest) are hard-pinned.
  std::uint32_t recencyTailTokens = 0;
  /// File paths the agent has edited. Tool results that mention these paths
  /// in their content are hard-pinned (the change history is sacred).
  std::unordered_set<std::string> editedFiles;
  /// Turn IDs the agent has explicitly pinned via the `pin` tool.
  std::unordered_set<std::string> agentPinnedTurnIds;
  /// Turn IDs referenced by an active todo or artifact (caller resolves).
  std::unordered_set<std::string> activeStateReferences;
};

/**
 * @brief Classify every turn in a history into pin kinds.
 *
 * Pure function. Stateless. Side-effect free. No I/O. The same inputs always
 * produce the same outputs, which makes pin behavior testable in isolation.
 *
 * Pairing pass: after the deterministic classification, this function walks
 * tool-call / tool-result pairs. If a hard-pinned turn has a tool_result
 * whose tool_call lives in an Evictable turn, the tool_call's turn is
 * upgraded to SoftPin. Same for the reverse direction. Cascades to fixed
 * point. This preserves the tool-call/result pairing invariant that
 * providers require.
 */
PinClassification classifyPins(const shared::AgentHistory& history,
                               const shared::AgentContext& context,
                               const PinPolicyInputs& inputs,
                               const shared::ITokenizer& tokenizer);

} // namespace firmius::core::working_memory

#endif

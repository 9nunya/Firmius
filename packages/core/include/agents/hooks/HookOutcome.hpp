#ifndef FIRMIUS_CORE_HOOKS_HOOK_OUTCOME_HPP
#define FIRMIUS_CORE_HOOKS_HOOK_OUTCOME_HPP

#include <map>
#include <optional>
#include <string>
#include <vector>

namespace firmius::core::hooks {

/**
 * @brief Structured outcome of a single hook action firing.
 *
 * Replaces the legacy "stdout becomes a system reminder" channel with a
 * richer surface. Hooks can:
 *   - Decide whether the originating tool/event should proceed.
 *   - Inject a system reminder for the agent's next turn.
 *   - Mutate hook state (KV writes).
 *   - Request a subagent spawn (filled in by Agent action kind).
 *   - Rewrite the originating tool's arguments (ToolIntercept action kind).
 *
 * All fields are optional. A no-op hook returns a default-constructed
 * HookOutcome with `decision == Decision::Allow` and nothing else set.
 *
 * Compatibility note (Claude Code / opencode):
 *   Shell hooks may emit a JSON object on stdout to populate this struct
 *   directly. The legacy stdout-as-reminder behavior remains when the
 *   stdout is non-JSON. See HookEnvelope for the wire format.
 */
struct HookOutcome {
  enum class Decision {
    Allow,    ///< proceed with the originating tool/event
    Block,    ///< reject the originating tool/event (blockable events only)
    Replace,  ///< proceed but with rewritten args (ToolIntercept)
  };

  // ─── Decision channel ─────────────────────────────────────────────────────
  Decision decision = Decision::Allow;
  std::string blockReason;            ///< user-facing reason when blocked
  std::string replacementToolArgs;    ///< JSON, only used when Decision::Replace

  // ─── Agent channel ────────────────────────────────────────────────────────
  /// Reminder text injected into the agent's next prompt as a
  /// `<FIRMIUS_HOOK ...>...</FIRMIUS_HOOK>` envelope.
  std::optional<std::string> reminderForAgent;

  // ─── State channel ────────────────────────────────────────────────────────
  /// Writes to apply to HookState after the action settles. Path syntax:
  /// `dotted.path` for objects, `array[]` for append. Atomic per outcome.
  struct StateWrite {
    std::string scope;   ///< "global" | "thread" | "agent" | "hook"
    std::string path;    ///< dotted path; trailing `[]` means append
    std::string valueJson;
  };
  std::vector<StateWrite> stateWrites;

  // ─── Subagent channel (Agent action kind) ─────────────────────────────────
  /// When non-empty, the dispatcher spawned a subagent and this outcome
  /// carries its return trophy (already JSON-decoded). Hook chains downstream
  /// can read `{{subagent.return.*}}` from this field.
  std::string spawnedAgentId;
  std::string spawnedAgentReturnJson;

  // ─── Diagnostics ──────────────────────────────────────────────────────────
  /// Free-form attribution + outcome label. The named outcome (e.g.
  /// "accept" | "reject" | "lie") is what `compose` chains match against.
  std::string outcomeLabel;
  std::map<std::string, std::string> tags;

  // ─── Tool return payload (workflow-defined tool invocation) ───────────────
  /// JSON object string. When non-empty, the tool executor will surface this
  /// under `return` in the ToolResult payload.
  std::string toolReturnPayloadJson;
};

} // namespace firmius::core::hooks

#endif

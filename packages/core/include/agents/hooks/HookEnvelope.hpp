#ifndef FIRMIUS_CORE_HOOKS_HOOK_ENVELOPE_HPP
#define FIRMIUS_CORE_HOOKS_HOOK_ENVELOPE_HPP

#include "agents/hooks/HookOutcome.hpp"
#include "agents/hooks/HookRegistry.hpp"

#include <optional>
#include <string>

namespace firmius::core::hooks {

/**
 * @brief Wire format for shell-hook stdin/stdout and the Luau script bridge.
 *
 * Firmius speaks two compatible dialects so users can port Claude Code /
 * opencode hook scripts with minimal change:
 *
 *   - On stdin (when `pass_envelope: true` in the YAML, default for new
 *     hooks): a JSON object containing the EventPayload, hook id, hook
 *     state snapshot, and Firmius-specific extras.
 *
 *   - On stdout: optional JSON object describing the HookOutcome. If the
 *     stdout starts with '{' we attempt to parse it; otherwise the legacy
 *     "stdout becomes a reminder" behavior applies.
 *
 *   - Exit codes: 0 = allow with optional reminder; 2 = block (Claude Code
 *     convention); other non-zero = soft fail (reminder injected, no block).
 *
 * The Luau script bridge speaks the same envelope shape as a Lua table,
 * keeping the mental model identical across action kinds.
 */
struct HookEnvelope {
  // ─── Identity ─────────────────────────────────────────────────────────────
  std::string hookId;
  std::string hookEvent;     ///< serialized WorkflowEventKind
  std::string firmiusVersion;

  // ─── Event payload (mirrors EventPayload, JSON-shaped) ────────────────────
  std::string threadId;
  std::string agentId;
  std::string persona;
  std::string activeMode;

  std::string toolName;
  std::string toolArgsJson;
  std::string toolResultJson;
  std::optional<bool> toolSuccess;

  std::string userMessage;
  std::string fromMode;
  std::string toMode;

  std::string completedWorkflowId;
  std::string subagentBranchId;
  std::string returnPayloadJson;

  std::map<std::string, std::string> extra;

  // ─── State snapshot (read-only view; writes go through HookOutcome) ───────
  /// JSON object: { "thread": {...}, "agent": {...}, "global": {...},
  ///                "hook": {...} }. Hooks read from this view and emit
  ///                StateWrite entries to mutate.
  std::string stateSnapshotJson;

  // ─── Compatibility flags ──────────────────────────────────────────────────
  /// When true, the hook runner mimics Claude Code's exit-code-based
  /// blocking convention (exit 2 = block) for shell hooks. Defaults true.
  bool claudeCodeCompat = true;
};

/**
 * @brief Serialize a HookEnvelope to the canonical JSON wire format.
 *
 * The shape is: { "hook_id": ..., "event": ..., "payload": {...},
 *                  "state": {...}, "firmius_version": "..." }.
 * Every field is present (empty-string for absent) so consumers can
 * read it without conditionals.
 */
std::string serializeEnvelope(const HookEnvelope &env);

/**
 * @brief Build a HookEnvelope from a fired EventPayload + state snapshot.
 *
 * Stateless helper used by the dispatcher before invoking shell or Luau
 * actions. The returned envelope is cheap to copy and serialize.
 */
HookEnvelope buildEnvelope(const std::string &hookId, WorkflowEventKind kind,
                           const EventPayload &payload,
                           const std::string &stateSnapshotJson);

/**
 * @brief Parse a hook outcome from the runner's stdout.
 *
 * Tries JSON first; falls back to the legacy "non-empty stdout becomes a
 * reminder, exit code 2 means block" convention when the body is not JSON.
 *
 * @param hookId           the firing hook (for envelope-attribution)
 * @param eventKind        for legacy reminder formatting
 * @param exitCode         the runner's exit code
 * @param stdoutBuf        captured stdout (may be empty)
 * @param stderrBuf        captured stderr (used as reminder fallback on fail)
 * @param claudeCodeCompat respect Claude Code exit-code 2 = block convention
 */
HookOutcome parseHookOutcome(const std::string &hookId,
                             WorkflowEventKind eventKind, int exitCode,
                             const std::string &stdoutBuf,
                             const std::string &stderrBuf,
                             bool claudeCodeCompat);

} // namespace firmius::core::hooks

#endif

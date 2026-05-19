#ifndef FIRMIUS_CORE_SCRIPTRUNTIME_HPP
#define FIRMIUS_CORE_SCRIPTRUNTIME_HPP

#include "agents/hooks/HookEnvelope.hpp"
#include "agents/hooks/HookOutcome.hpp"

#include <chrono>
#include <cstdint>
#include <cstddef>
#include <memory>
#include <string>

namespace firmius::core::hooks {

/**
 * @brief Resource budget for one Luau hook script evaluation.
 *
 * Lifted out of ScriptRuntime so the type is fully complete before any
 * member function references it (avoids "default member initializer
 * needed within definition of enclosing class" diagnostics).
 */
struct ScriptLimits {
  std::uint64_t maxInstructions = 1'000'000;
  std::chrono::milliseconds wallClockTimeout{0};
  std::size_t memoryBudgetBytes = 8 * 1024 * 1024;
};

/**
 * @brief Sandboxed Luau runtime that evaluates `kind: script` hook actions.
 *
 * Why Luau (not Lua):
 *   - Sandbox-by-default: no `loadstring`, `getfenv`, `setfenv`, `dofile`,
 *     `loadfile`, `package`, `require`, `os.execute`, `io.*`. The unsafe
 *     surface is gone at the language level, not just by whitelist.
 *   - Deterministic-friendly: no `math.random` global state across VMs;
 *     every script gets its own L state.
 *   - Native typechecker available for later authoring tooling.
 *   - C API is a near-superset of Lua 5.1, so binding effort is small.
 *
 * Hook script contract (the API a hook author writes against):
 *
 *   -- The `event` table is the parsed HookEnvelope.
 *   -- The `state` API reads/writes HookState through structured calls.
 *   -- `agent.spawn(persona, task, opts)` is the Agent-action equivalent:
 *      it waits for the subagent and returns a table with text/json fields.
 *   -- `thread.history/messages/tool_calls/tool_results(filter)` exposes
 *      transcript and tool evidence to validators.
 *   -- `outcome.allow{ reminder = "..." }`, `outcome.block{ reason = "..." }`,
 *      `outcome.replace{ args = {...} }` set the decision.
 *   -- Returning the outcome table finalises and emits a HookOutcome.
 *
 * Each script runs in a fresh Luau state; cross-script communication goes
 * through HookState. State globals are read-only from the script's point
 * of view (`lua_setreadonly(L, idx, true)` on the env table).
 *
 * Memory + time budgets:
 *   - Hard cap on instructions executed per evaluation (debug hook fires
 *     after N opcodes, raises a Luau error). Default 1,000,000.
 *   - Optional wall-clock timeout: `0ms` disables it. The default is disabled
 *     because hook scripts may legitimately wait on runtime bridges such as
 *     `agent.spawn(...)`; runaway in-VM execution is constrained by the
 *     instruction budget instead.
 *   - Memory cap via `lua_callbacks(L)->onallocate`. Default 8MB.
 */
class ScriptRuntime {
public:
  using Limits = ScriptLimits;  ///< back-compat alias for callers

  /// Build a runtime configured with the given limits. The runtime is
  /// reusable across many `eval()` calls; each call gets a fresh sandboxed
  /// state internally.
  static std::unique_ptr<ScriptRuntime> create(const ScriptLimits &limits =
                                                   ScriptLimits{});

  virtual ~ScriptRuntime() = default;

  /// Evaluate a Luau hook body against `env`. The script's return value
  /// (a Lua table shaped like a HookOutcome) is converted into a real
  /// HookOutcome.
  ///
  /// Errors: a script error returns a HookOutcome with
  ///   decision = Allow,
  ///   reminderForAgent = "<FIRMIUS_HOOK ... exit=\"N\">script error: ...",
  ///   tags["script_error"] = "...".
  /// We intentionally do NOT block on script errors — a buggy hook should
  /// not brick the agent.
  virtual HookOutcome eval(const std::string &hookId,
                           const std::string &scriptBody,
                           const HookEnvelope &env) = 0;

  /// True when the build was configured with `FIRMIUS_ENABLE_LUAU_HOOKS`.
  /// When false, `create()` returns a no-op runtime that always emits an
  /// Allow outcome with a "luau disabled at build time" reminder.
  static bool enabled();

protected:
  ScriptRuntime() = default;
};

} // namespace firmius::core::hooks

#endif

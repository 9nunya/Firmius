#ifndef FIRMIUS_CORE_HOOKS_HOOK_REGISTRY_HPP
#define FIRMIUS_CORE_HOOKS_HOOK_REGISTRY_HPP

#include "agents/hooks/HookOutcome.hpp"
#include "workflow/Workflow.hpp"

#include <functional>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace firmius::core::hooks {

/**
 * @brief Event payload threaded through the dispatcher.
 *
 * Each event kind reads a relevant subset of fields. A blockable event
 * (pre_tool_use, user_message, thread_start, thread_resume) honours
 * `result.blocked = true` to short-circuit the originating action.
 *
 * Fields are intentionally strings for portability — match predicates in
 * hook YAML compare strings; structured types would force per-event-kind
 * matchers and complicate the dispatcher table.
 */
struct EventPayload {
  std::string threadId;
  std::string agentId;
  std::string persona;
  std::string activeMode;

  // pre_tool_use / post_tool_use
  std::string toolName;
  std::string toolArgsJson;
  std::string toolResultJson;
  std::optional<bool> toolSuccess;

  // user_message
  std::string userMessage;

  // mode events
  std::string fromMode;
  std::string toMode;

  // workflow_complete / subagent_return
  std::string completedWorkflowId;
  std::string subagentBranchId;
  std::string returnPayloadJson;

  // open-ended attributes for any custom matcher
  std::map<std::string, std::string> extra;
};

/**
 * @brief Aggregated outcome of firing all matching hooks for one event.
 *
 * `injectedReminders` are concatenated and surface to the agent as a
 * system reminder turn (the same channel runtime nudges already use).
 * `blocked` is honoured only by blockable events.
 */
struct EventResult {
  bool blocked = false;
  std::string blockReason;
  std::string replacementToolArgs;
  std::optional<HookOutcome> firstOutcome;
  std::vector<std::string> injectedReminders;
  std::vector<std::string> firedHookIds;  ///< for observability / TUI bands
};

struct HookActivityRecord {
  std::string hookId;
  std::string threadId;
  std::string agentId;
  std::string eventName;
  std::string decision;
  std::string outcomeLabel;
  std::string blockReason;
  std::string statusLine;
  std::uint64_t timestampMs = 0;
  int stateWriteCount = 0;
};

/**
 * @brief Registry of event-triggered hooks.
 *
 * A "hook" is just a Workflow whose `trigger.kind == OnEvent`. The registry
 * pulls those out of WorkflowLoader on `reload()` and indexes them by
 * event kind for cheap dispatch. Refresh after editing hook YAML by
 * calling `reload()` (no daemon restart required).
 */
class HookRegistry {
public:
  static HookRegistry &instance();

  /// Re-scan WorkflowLoader for OnEvent workflows. Idempotent.
  void reload();

  /// All hooks for a given event kind.
  std::vector<const Workflow *> hooksFor(WorkflowEventKind kind) const;

  /// Total registered hook count.
  std::size_t size() const;

  /// Pack capability gate used by HookState/script helpers.
  bool isStateAccessAllowed(const std::string &hookId, const std::string &scope,
                            const std::string &path) const;

  /// Recent hook firings for a thread, newest last.
  std::vector<HookActivityRecord>
  recentActivity(const std::string &threadId,
                 std::size_t maxCount = 24) const;

  /// Append a record to the thread activity log.
  void recordActivity(HookActivityRecord record);

private:
  HookRegistry() = default;
  mutable std::mutex mu_;
  std::map<WorkflowEventKind, std::vector<const Workflow *>> byEvent_;
  std::map<std::string, const Workflow *> byId_;
  std::map<std::string, std::vector<HookActivityRecord>> activityByThread_;
};

// ─── Dispatcher ─────────────────────────────────────────────────────────────
// The dispatcher is stateless: it queries HookRegistry, evaluates the
// match predicate, executes the action, and aggregates outcomes. Hosted
// on a static class for ergonomic Agent.cpp call sites.

class HookDispatcher {
public:
  /// Fire an event. Synchronous. Returns aggregated result.
  static EventResult fire(WorkflowEventKind kind, const EventPayload &payload);

  /// Test seam — override the runner used to execute Shell actions.
  using ShellRunner = std::function<int(const std::string &command,
                                        const std::string &stdinPayload,
                                        int timeoutSec, std::string *stdoutOut,
                                        std::string *stderrOut)>;
  static void setShellRunner(ShellRunner runner);

  /// Executes the hook's action (no match predicate, no registry lookup).
  /// Public so non-event surfaces (e.g. workflow-defined tools) can reuse the
  /// exact action machinery.
  static HookOutcome runAction(const Workflow &hook,
                               const EventPayload &payload);

  /// Applies side-effects (state writes) for a hook outcome.
  static void settleOutcome(const Workflow &hook, HookOutcome &out);

private:
  static bool matches(const Workflow &hook, const EventPayload &payload);
};

} // namespace firmius::core::hooks

#endif

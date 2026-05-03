#include "commands/HooksCommand.hpp"

#include "AgentRegistry.hpp"
#include "agents/hooks/HookRegistry.hpp"
#include "agents/hooks/HookState.hpp"
#include "harness/Harness.hpp"
#include "workflow/WorkflowLoader.hpp"

#include <iostream>

namespace firmius::tui {
namespace {

std::string commandName(const firmius::core::Workflow &workflow) {
  if (workflow.slashCommand.has_value() && !workflow.slashCommand->empty()) {
    return *workflow.slashCommand;
  }
  return "/" + workflow.id;
}

std::string actionKind(const firmius::core::Workflow &workflow) {
  return firmius::core::workflowActionKindToString(workflow.action.kind);
}

void printList() {
  auto &loader = firmius::core::WorkflowLoader::instance();
  auto workflows = loader.getAllWorkflows();

  std::cout << "# Hooks\n\n";
  std::cout << "Registered event hooks: "
            << firmius::core::hooks::HookRegistry::instance().size() << "\n\n";

  std::cout << "## Event Hooks\n";
  bool sawHook = false;
  for (const auto &workflow : workflows) {
    if (!workflow.isHook()) {
      continue;
    }
    sawHook = true;
    std::cout << "- " << workflow.id << " event="
              << firmius::core::workflowEventKindToString(workflow.trigger.event)
              << " action=" << actionKind(workflow)
              << " block=" << (workflow.trigger.block ? "yes" : "no")
              << "\n  " << workflow.sourcePath << "\n";
  }
  if (!sawHook) {
    std::cout << "- (none)\n";
  }

  std::cout << "\n## Slash Workflows\n";
  bool sawSlash = false;
  for (const auto &workflow : workflows) {
    if (!workflow.slashCommand.has_value()) {
      continue;
    }
    sawSlash = true;
    std::cout << "- " << commandName(workflow) << " id=" << workflow.id
              << " action=" << actionKind(workflow)
              << (workflow.rawRemainder ? " raw_remainder=yes" : "")
              << "\n  " << workflow.sourcePath << "\n";
  }
  if (!sawSlash) {
    std::cout << "- (none)\n";
  }
}

} // namespace

void HooksCommand::execute(CommandCtx &, const std::vector<ParsedArg> &args) {
  const std::string action =
      args.empty() || args[0].raw_value.empty() ? "list" : args[0].raw_value;

  if (action == "reload") {
    firmius::core::WorkflowLoader::instance().init();
    firmius::core::hooks::HookRegistry::instance().reload();
    std::cout << "Hooks reloaded. Registered event hooks: "
              << firmius::core::hooks::HookRegistry::instance().size() << "\n";
    return;
  }

  if (action == "state") {
    const std::string threadId = firmius::core::Harness::instance().currentThreadId();
    if (threadId.empty()) {
      std::cout << "No active thread.\n";
      return;
    }
    auto &state = firmius::core::hooks::HookState::instance();
    state.bindThread(threadId);
    std::cout << state.snapshotJson("hooks.command") << "\n";
    return;
  }

  if (action == "fire") {
    const std::string eventName =
        args.size() > 1 && !args[1].raw_value.empty() ? args[1].raw_value
                                                      : "agent_stop";
    auto kind = firmius::core::workflowEventKindFromString(eventName);
    firmius::core::hooks::EventPayload payload;
    payload.threadId = firmius::core::Harness::instance().currentThreadId();
    payload.agentId = firmius::core::Harness::instance().focusedAgentId();
    if (auto agent =
            firmius::core::AgentRegistry::instance().getAgent(payload.agentId)) {
      payload.persona = agent->getContext().config.personaName;
      payload.activeMode = agent->getContext().state.activeMode;
    }
    auto result = firmius::core::hooks::HookDispatcher::fire(kind, payload);
    std::cout << "event=" << eventName
              << " blocked=" << (result.blocked ? "yes" : "no") << "\n";
    if (!result.blockReason.empty()) {
      std::cout << "reason=" << result.blockReason << "\n";
    }
    for (const auto &id : result.firedHookIds) {
      std::cout << "- fired " << id << "\n";
    }
    for (const auto &reminder : result.injectedReminders) {
      std::cout << "\n" << reminder << "\n";
    }
    return;
  }

  printList();
}

} // namespace firmius::tui

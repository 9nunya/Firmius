#include "tools/ModeSwitchTool.hpp"

#include "IAgent.hpp"
#include "agents/hooks/HookRegistry.hpp"
#include "agents/modes/Mode.hpp"
#include "utils/StringUtil.hpp"

#include <algorithm>
#include <sstream>

namespace firmius::core {

using shared::JSONSchema;
using shared::ToolContext;
using shared::ToolMetadata;
using shared::ToolResult;
using shared::ToolScope;

namespace {

// Fire mode_exited / mode_entered hooks. Each fired hook may inject a
// reminder; the dispatcher returns them aggregated. We surface the firing
// summary in the tool result so the model sees what happened in plain text.
void fireModeEvent(WorkflowEventKind kind, ToolContext &ctx,
                   const std::string &fromMode, const std::string &toMode,
                   std::vector<std::string> &reminders) {
  auto &agentCtx = ctx.agent.getContext();
  hooks::EventPayload payload;
  payload.threadId = agentCtx.history ? agentCtx.history->threadId : "";
  payload.agentId = agentCtx.identity.id;
  payload.persona = agentCtx.config.personaName;
  payload.activeMode = toMode;
  payload.fromMode = fromMode;
  payload.toMode = toMode;
  auto fired = hooks::HookDispatcher::fire(kind, payload);
  for (auto &r : fired.injectedReminders) {
    reminders.push_back(std::move(r));
  }
}

bool transitionAllowed(const modes::Mode &current, const std::string &target) {
  // Empty allowed_transitions_to = no constraint (any transition allowed).
  if (current.allowedTransitionsTo.empty()) {
    return true;
  }
  return std::find(current.allowedTransitionsTo.begin(),
                   current.allowedTransitionsTo.end(),
                   target) != current.allowedTransitionsTo.end();
}

} // namespace

ToolMetadata ModeSwitchTool::getMetadata() const {
  return {"ModeSwitch",
          "Switch the calling agent's active mode. Modes are operational "
          "stances that overlay a sub-prompt, scope tools, and define the "
          "expected return shape. Bare names resolve against the active "
          "persona's sub-modes first; qualified `persona:submode` is "
          "verbatim. Pass an empty name to clear the active mode.",
          ToolScope::Semantic};
}

std::shared_ptr<JSONSchema> ModeSwitchTool::getSchema() const {
  return shared::zObject({
             {"name", shared::zString()->describe(
                          "Mode to enter. Either a bare sub-mode (e.g. "
                          "'apply' for the active persona) or a qualified "
                          "form (e.g. 'forge:apply', 'diagnose'). Empty "
                          "string clears the active mode.")},
             {"reason", shared::zString()->setOptional()->describe(
                            "Optional human-readable rationale for the "
                            "switch. Surfaced in event payloads and the "
                            "TUI status band.")},
         })
      ->required({"name"});
}

ToolResult ModeSwitchTool::execute(const ModeSwitchInput &input,
                                   ToolContext &ctx) {
  try {
    auto &mutableCtx = ctx.agent.getMutableContext();
    const std::string personaName = mutableCtx.config.personaName;
    const std::string previousMode = mutableCtx.state.activeMode;

    // Empty name = clear active mode. Useful when an agent finishes its
    // last sub-mode and wants to return to a "no overlay" base stance.
    const std::string requested = shared::StringUtil::trim(input.name);
    if (requested.empty()) {
      std::vector<std::string> reminders;
      if (!previousMode.empty()) {
        fireModeEvent(WorkflowEventKind::ModeExited, ctx, previousMode, "",
                      reminders);
      }
      mutableCtx.state.activeMode.clear();

      rapidjson::Document doc;
      doc.SetObject();
      auto &alloc = doc.GetAllocator();
      // Token-waste pass 5: prose-first mode-cleared response. Dropped
      // to_mode (always empty here) and cleared (derivable). from_mode
      // is only useful when non-empty.
      std::ostringstream prose;
      if (previousMode.empty()) {
        prose << "Active mode cleared (no mode was set).";
      } else {
        prose << "Cleared active mode (was " << previousMode << ").";
      }
      const std::string proseStr = prose.str();
      doc.AddMember(
          "result",
          rapidjson::Value(proseStr.c_str(),
                           static_cast<rapidjson::SizeType>(proseStr.size()),
                           alloc).Move(),
          alloc);
      if (!previousMode.empty()) {
        doc.AddMember("from_mode",
                      rapidjson::Value(previousMode.c_str(), alloc), alloc);
      }
      if (!reminders.empty()) {
        rapidjson::Value remArr(rapidjson::kArrayType);
        for (const auto &r : reminders) {
          remArr.PushBack(rapidjson::Value(r.c_str(), alloc), alloc);
        }
        doc.AddMember("hook_reminders", remArr, alloc);
      }
      return ToolResult::ok(doc);
    }

    auto &registry = modes::ModeRegistry::instance();
    const auto *target = registry.resolveForPersona(requested, personaName);
    if (target == nullptr) {
      std::ostringstream err;
      err << "Mode not found: " << requested;
      if (!personaName.empty()) {
        err << " (active persona: " << personaName << ")";
      }
      err << ". Registered modes: ";
      const auto names = registry.listNames();
      for (std::size_t i = 0; i < names.size(); ++i) {
        if (i > 0) err << ", ";
        err << names[i];
      }
      err << ".";
      throw std::runtime_error(err.str());
    }

    // Persona-scope enforcement: a sub-mode owned by another persona is
    // not switchable by this agent.
    if (target->isPersonaScoped() && target->personaScope.has_value() &&
        *target->personaScope != personaName) {
      throw std::runtime_error(
          "Mode '" + target->qualifiedName() + "' is scoped to persona '" +
          *target->personaScope + "' but the active persona is '" +
          personaName + "'. Spawn a delegate of that persona instead.");
    }

    // Transition enforcement: when current mode constrains its outgoing
    // transitions, honour the constraint.
    if (!previousMode.empty()) {
      if (const auto *current =
              registry.resolveForPersona(previousMode, personaName)) {
        if (!transitionAllowed(*current, target->qualifiedName()) &&
            !transitionAllowed(*current, target->name)) {
          std::ostringstream err;
          err << "Transition from '" << current->qualifiedName() << "' to '"
              << target->qualifiedName()
              << "' is not allowed. Allowed targets: ";
          for (std::size_t i = 0; i < current->allowedTransitionsTo.size();
               ++i) {
            if (i > 0) err << ", ";
            err << current->allowedTransitionsTo[i];
          }
          err << ".";
          throw std::runtime_error(err.str());
        }
      }
    }

    // Commit the switch.
    const std::string targetQualified = target->qualifiedName();
    mutableCtx.state.activeMode = targetQualified;
    if (mutableCtx.state.initialMode.empty()) {
      mutableCtx.state.initialMode = targetQualified;
    }

    // Fire mode_exited (old) + mode_entered (new). Hooks may inject
    // reminders that surface in the tool result for the agent to read.
    std::vector<std::string> reminders;
    if (!previousMode.empty()) {
      fireModeEvent(WorkflowEventKind::ModeExited, ctx, previousMode,
                    targetQualified, reminders);
    }
    fireModeEvent(WorkflowEventKind::ModeEntered, ctx, previousMode,
                  targetQualified, reminders);

    rapidjson::Document doc;
    doc.SetObject();
    auto &alloc = doc.GetAllocator();
    // Token-waste pass 5: prose-first mode-switch result. The mode title
    // and stance are folded into the prose; expected_return_shape (a
    // schema string the model needs to read literally) and
    // allowed_next_modes (a constraint list the model needs to obey)
    // stay structured. from_mode is dropped when empty.
    std::ostringstream prose;
    prose << "Switched to " << targetQualified;
    if (!target->title.empty()) {
      prose << " (" << target->title << ")";
    }
    if (!target->shortDescription.empty()) {
      prose << ". " << target->shortDescription;
    } else {
      prose << ".";
    }
    if (input.reason.has_value() && !input.reason->empty()) {
      prose << " Reason: " << *input.reason << ".";
    }
    const std::string proseStr = prose.str();
    doc.AddMember(
        "result",
        rapidjson::Value(proseStr.c_str(),
                         static_cast<rapidjson::SizeType>(proseStr.size()),
                         alloc).Move(),
        alloc);
    doc.AddMember("to_mode",
                  rapidjson::Value(targetQualified.c_str(), alloc), alloc);
    if (!previousMode.empty()) {
      doc.AddMember("from_mode",
                    rapidjson::Value(previousMode.c_str(), alloc), alloc);
    }
    if (target->outputSchema.has_value() && !target->outputSchema->empty()) {
      doc.AddMember("expected_return_shape",
                    rapidjson::Value(target->outputSchema->c_str(), alloc),
                    alloc);
    }
    if (!target->allowedTransitionsTo.empty()) {
      rapidjson::Value arr(rapidjson::kArrayType);
      for (const auto &t : target->allowedTransitionsTo) {
        arr.PushBack(rapidjson::Value(t.c_str(), alloc), alloc);
      }
      doc.AddMember("allowed_next_modes", arr, alloc);
    }
    if (!reminders.empty()) {
      rapidjson::Value remArr(rapidjson::kArrayType);
      for (const auto &r : reminders) {
        remArr.PushBack(rapidjson::Value(r.c_str(), alloc), alloc);
      }
      doc.AddMember("hook_reminders", remArr, alloc);
    }
    return ToolResult::ok(doc);
  } catch (const std::exception &e) {
    return ToolResult::fail(e.what());
  }
}

} // namespace firmius::core

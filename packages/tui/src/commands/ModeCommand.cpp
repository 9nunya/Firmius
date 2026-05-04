#include "commands/ModeCommand.hpp"

#include "AgentRegistry.hpp"
#include "NotificationManager.hpp"
#include "TUIState.hpp"
#include "agents/modes/Mode.hpp"
#include "utils/ModeCycle.hpp"

#include <chrono>

namespace firmius::tui {

namespace {

// "none", "clear", "off" all map to the empty mode (no overlay).
bool isClearToken(const std::string &s) {
  return s == "none" || s == "clear" || s == "off";
}

// Resolve a bare or qualified mode name through the registry, scoped to a
// persona. Returns empty string when not found; otherwise returns the
// qualified name (so the status bar shows the full form).
std::string resolveMode(const std::string &requested,
                        const std::string &personaName) {
  auto &reg = firmius::core::modes::ModeRegistry::instance();
  if (const auto *m = reg.resolveForPersona(requested, personaName)) {
    return m->qualifiedName();
  }
  return "";
}

} // namespace

void ModeCommand::execute(CommandCtx &ctx, const std::vector<ParsedArg> &args) {
  auto &state = *ctx.state;

  const std::string token = args.empty() ? std::string{} : args[0].asString();

  // Welcome path: mutate pre-thread initial mode.
  if (state.getViewMode() == TuiState::ViewMode::Welcome) {
    const std::string persona = state.thread_.leadPersona.empty()
                                    ? std::string("aster")
                                    : state.thread_.leadPersona;
    std::string next;
    if (token.empty()) {
      next = cycleMode(state.thread_.initialMode, persona, +1);
    } else if (isClearToken(token)) {
      next = "";
    } else {
      next = resolveMode(token, persona);
      if (next.empty()) {
        NotificationManager::instance().notifyWarning(
            "Mode", "Unknown mode '" + token + "' for persona '" + persona +
                        "'. Use Ctrl+Y or `/mode` to cycle.",
            std::chrono::milliseconds(2500));
        return;
      }
    }
    state.thread_.initialMode = next;
    NotificationManager::instance().notifyInfo(
        "Mode",
        next.empty() ? "Initial mode cleared (no overlay)."
                     : "Initial mode: " + next,
        std::chrono::milliseconds(1500));
    state.postEvent(ftxui::Event::Custom);
    return;
  }

  // Mid-thread path: mutate focused agent's state.activeMode.
  auto agent =
      firmius::core::AgentRegistry::instance().getAgent(state.focused_agent_id_);
  if (!agent) {
    NotificationManager::instance().notifyWarning(
        "Mode", "No focused agent.", std::chrono::milliseconds(1500));
    return;
  }
  auto &agentCtx = agent->getMutableContext();
  const std::string persona = agentCtx.config.personaName;

  std::string next;
  if (token.empty()) {
    next = cycleMode(agentCtx.state.activeMode, persona, +1);
  } else if (isClearToken(token)) {
    next = "";
  } else {
    next = resolveMode(token, persona);
    if (next.empty()) {
      NotificationManager::instance().notifyWarning(
          "Mode", "Unknown mode '" + token + "' for persona '" + persona +
                      "'. Use Ctrl+Y to cycle.",
          std::chrono::milliseconds(2500));
      return;
    }
  }

  agentCtx.state.activeMode = next;
  NotificationManager::instance().notifyInfo(
      "Mode",
      next.empty() ? "Mode cleared on " + agentCtx.identity.friendlyName
                   : "Mode: " + next + " (" + agentCtx.identity.friendlyName +
                         ")",
      std::chrono::milliseconds(1500));
  state.postEvent(ftxui::Event::Custom);
}

} // namespace firmius::tui

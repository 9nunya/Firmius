#include "AgentTabBar.hpp"
#include "Terminal.hpp"
#include "ThemeManager.hpp"

#include <algorithm>
#include <sstream>

namespace firmius::tui {

AgentTabBar::AgentTabBar(const AppState& state) : state_(state) {}

int AgentTabBar::height(int /*width*/) const { return 1; }

std::vector<std::string> AgentTabBar::render(int width) const {
  const auto& theme = ThemeManager::instance().currentTheme();
  auto agents = state_.agentList();
  std::string focusedId = state_.focusedAgentId();

  // Color palette for agent pills
  std::string line;

  for (const auto& agent : agents) {

    bool focused = (agent.agentId == focusedId);
    bool active = agent.running &&
                  (agent.status == firmius::shared::AgentStatus::Streaming ||
                   agent.status == firmius::shared::AgentStatus::ExecutingTool ||
                   agent.status == firmius::shared::AgentStatus::ProviderWaiting);

    // Build pill label — prefer the operator-facing friendly name.
    std::string label;
    if (!agent.friendlyName.empty()) {
      label = agent.friendlyName;
    } else if (!agent.title.empty()) {
      label = agent.title;
    } else {
      label = agent.agentId.substr(0, 8);
    }

    // Truncate label
    if (label.size() > 20) label = label.substr(0, 18) + "..";

    // State indicator
    std::string stateStr;
    if (active) {
      switch (agent.status) {
      case firmius::shared::AgentStatus::Streaming:
        stateStr = " *";
        break;
      case firmius::shared::AgentStatus::ExecutingTool:
        stateStr = " @";
        break;
      case firmius::shared::AgentStatus::ProviderWaiting:
        stateStr = " ~";
        break;
      default:
        stateStr = " +";
        break;
      }
    } else if (agent.status == firmius::shared::AgentStatus::Error) {
      stateStr = " !";
    } else if (!agent.running && agent.booting) {
      stateStr = " ..";
    }

    // Compose the pill
    std::string pill = " " + label + stateStr + " ";

    if (focused) {
      line += ansi::bgRgb(theme.agentStrip.item.focused.bg.r,
                          theme.agentStrip.item.focused.bg.g,
                          theme.agentStrip.item.focused.bg.b,
                          ansi::fgRgb(theme.agentStrip.item.focused.fg.r,
                                      theme.agentStrip.item.focused.fg.g,
                                      theme.agentStrip.item.focused.fg.b, pill));
    } else if (active) {
      line += ansi::fgRgb(theme.agentStrip.item.busy.fg.r,
                          theme.agentStrip.item.busy.fg.g,
                          theme.agentStrip.item.busy.fg.b, pill);
    } else if (agent.status == firmius::shared::AgentStatus::Error) {
      line += ansi::fgRgb(theme.agentStrip.item.error.fg.r,
                          theme.agentStrip.item.error.fg.g,
                          theme.agentStrip.item.error.fg.b, pill);
    } else {
      line += ansi::dim(pill);
    }

    // Separator between pills
    if (&agent != &agents.back()) {
      line += ansi::dim("|");
    }
  }

  // Keybind hints at the right edge
  std::string hints = " ^N/^B next/prev ^P parent ";
  std::string padded = ansi::fitToWidth(line, width - static_cast<int>(hints.size())) +
                       ansi::dim(hints);

  // Ensure total width
  return {ansi::bgRgb(theme.agentStrip.bg.r, theme.agentStrip.bg.g,
                      theme.agentStrip.bg.b, ansi::fitToWidth(padded, width))};
}

} // namespace firmius::tui

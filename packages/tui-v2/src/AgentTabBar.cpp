#include "AgentTabBar.hpp"
#include "Terminal.hpp"

#include <algorithm>
#include <sstream>

namespace firmius::tui2 {

AgentTabBar::AgentTabBar(const AppState& state) : state_(state) {}

int AgentTabBar::height(int /*width*/) const { return 1; }

std::vector<std::string> AgentTabBar::render(int width) const {
  auto agents = state_.agentList();
  std::string focusedId = state_.focusedAgentId();

  // Color palette for agent pills
  static const int colors[][3] = {
    {100, 180, 255}, // blue
    {100, 220, 100}, // green
    {220, 180, 60},  // yellow
    {220, 100, 100}, // red
    {180, 100, 220}, // purple
    {100, 220, 220}, // cyan
  };
  constexpr int numColors = sizeof(colors) / sizeof(colors[0]);

  std::string line;
  int colorIdx = 0;

  for (const auto* agent : agents) {
    if (!agent) continue;

    bool focused = (agent->agentId == focusedId);
    bool active = agent->running &&
                  (agent->status == firmius::shared::AgentStatus::Streaming ||
                   agent->status == firmius::shared::AgentStatus::ExecutingTool ||
                   agent->status == firmius::shared::AgentStatus::ProviderWaiting);

    // Build pill label
    std::string label;
    if (!agent->friendlyName.empty()) {
      label = agent->friendlyName;
    } else if (!agent->title.empty()) {
      label = agent->title;
    } else if (!agent->personaName.empty()) {
      label = agent->personaName;
    } else {
      label = agent->agentId.substr(0, 8);
    }

    // Truncate label
    if (label.size() > 20) label = label.substr(0, 18) + "..";

    // State indicator
    std::string stateStr;
    if (active) {
      switch (agent->status) {
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
    } else if (agent->status == firmius::shared::AgentStatus::Error) {
      stateStr = " !";
    } else if (!agent->running && agent->booting) {
      stateStr = " ..";
    }

    // Compose the pill
    std::string pill = " " + label + stateStr + " ";

    int cr = colors[colorIdx % numColors][0];
    int cg = colors[colorIdx % numColors][1];
    int cb = colors[colorIdx % numColors][2];

    if (focused) {
      line += ansi::bgRgb(cr, cg, cb, ansi::fgRgb(0, 0, 0, pill));
    } else if (active) {
      line += ansi::fgRgb(cr, cg, cb, pill);
    } else {
      line += ansi::dim(pill);
    }

    // Separator between pills
    if (agent != agents.back()) {
      line += ansi::dim("|");
    }

    ++colorIdx;
  }

  // Keybind hints at the right edge
  std::string hints = " ^N/^B next/prev ^P parent ";
  std::string padded = ansi::fitToWidth(line, width - static_cast<int>(hints.size())) +
                       ansi::dim(hints);

  // Ensure total width
  return {ansi::bgRgb(18, 18, 28, ansi::fitToWidth(padded, width))};
}

} // namespace firmius::tui2

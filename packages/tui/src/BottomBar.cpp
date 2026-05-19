#include "BottomBar.hpp"
#include "Terminal.hpp"
#include "ThemeAnsi.hpp"
#include "ThemeManager.hpp"

namespace firmius::tui {

BottomBar::BottomBar(const AppState& state) : state_(state) {}

int BottomBar::height(int /*width*/) const { return 1; }

std::vector<std::string> BottomBar::render(int width) const {
  auto context = state_.activityContext();
  std::string hints;

  switch (context) {
  case ActivityContext::PermissionPending: {
    auto perm = state_.pendingPermission();
    std::string permTitle = perm ? perm->title : "Permission required";
    hints = theme_ansi::warning(" " + permTitle + " ") +
            ansi::dim("│") +
            theme_ansi::success(" [y]") + ansi::dim(" Allow ") +
            theme_ansi::error(" [n]") + ansi::dim(" Deny");
    if (perm && perm->allowAlways) {
      hints += theme_ansi::accent(" [a]") + ansi::dim(" Always");
    }
    break;
  }
  case ActivityContext::Active:
    hints = ansi::dim(" Esc") + ansi::dim(" Interrupt") +
            ansi::dim(" │ ") +
            ansi::dim(" Ctrl+T") + ansi::dim(" Todos") +
            ansi::dim(" │ ") +
            ansi::dim(" Ctrl+Q") + ansi::dim(" Quit");
    break;
  case ActivityContext::Idle:
    if (state_.hasMultipleAgents()) {
      hints = ansi::dim(" Ctrl+N/B") + ansi::dim(" Agents") +
              ansi::dim(" \xe2\x94\x82 ") +
              ansi::dim(" Ctrl+P") + ansi::dim(" Parent") +
              ansi::dim(" \xe2\x94\x82 ") +
              ansi::dim(" Ctrl+T") + ansi::dim(" Todos") +
              ansi::dim(" \xe2\x94\x82 ") +
              ansi::dim(" /resume") + ansi::dim(" Threads") +
              ansi::dim(" \xe2\x94\x82 ") +
              ansi::dim(" Ctrl+Q") + ansi::dim(" Quit");
    } else {
      hints = ansi::dim(" /resume") + ansi::dim(" Threads") +
              ansi::dim(" \xe2\x94\x82 ") +
              ansi::dim(" Ctrl+T") + ansi::dim(" Todos") +
              ansi::dim(" \xe2\x94\x82 ") +
              ansi::dim(" /models") + ansi::dim(" Switch") +
              ansi::dim(" \xe2\x94\x82 ") +
              ansi::dim(" Ctrl+Q") + ansi::dim(" Quit");
    }
    break;
  }

  // Use base.bg (the chat bg) so the keybind hint row blends with the rest of
  // the screen rather than appearing as a different-colored panel strip.
  // Inner content uses only fg-style helpers (fgRgb / dim) which never reset
  // the bg, so the outer bg holds across the entire line.
  const auto& theme = ThemeManager::instance().currentTheme();
  return {theme_ansi::bg(theme.base.bg, ansi::fitToWidth(hints, width))};
}

} // namespace firmius::tui

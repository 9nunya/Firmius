#include "BottomBar.hpp"
#include "Terminal.hpp"

namespace firmius::tui2 {

BottomBar::BottomBar(const AppState& state) : state_(state) {}

int BottomBar::height(int /*width*/) const { return 1; }

std::vector<std::string> BottomBar::render(int width) const {
  auto context = state_.activityContext();
  std::string hints;

  switch (context) {
  case ActivityContext::PermissionPending: {
    auto perm = state_.pendingPermission();
    std::string permTitle = perm ? perm->title : "Permission required";
    hints = ansi::fgRgb(220, 180, 60, " " + permTitle + " ") +
            ansi::dim("│") +
            ansi::fgRgb(100, 220, 100, " [y]") + ansi::dim(" Allow ") +
            ansi::fgRgb(220, 60, 60, " [n]") + ansi::dim(" Deny");
    if (perm && perm->allowAlways) {
      hints += ansi::fgRgb(100, 180, 255, " [a]") + ansi::dim(" Always");
    }
    break;
  }
  case ActivityContext::Streaming:
    hints = ansi::dim(" Esc") + ansi::dim(" Interrupt") +
            ansi::dim(" │ ") +
            ansi::dim(" Ctrl+Q") + ansi::dim(" Quit");
    break;
  case ActivityContext::Idle:
    hints = ansi::dim(" Ctrl+N") + ansi::dim(" New") +
            ansi::dim(" │ ") +
            ansi::dim(" /resume") + ansi::dim(" Threads") +
            ansi::dim(" │ ") +
            ansi::dim(" /models") + ansi::dim(" Switch") +
            ansi::dim(" │ ") +
            ansi::dim(" Ctrl+Q") + ansi::dim(" Quit");
    break;
  }

  return {ansi::bgRgb(20, 20, 30, ansi::fitToWidth(hints, width))};
}

} // namespace firmius::tui2

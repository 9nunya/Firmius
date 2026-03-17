#include "modals/ThreadLockedModal.hpp"
#include "TUIState.hpp"
#include "ThemeManager.hpp"
#include "modals/ModalLayout.hpp"
#include <ftxui/component/component.hpp>
#include <ftxui/dom/elements.hpp>

namespace firmius::tui {

ftxui::Component ThreadLockedModal::create(TuiState &state,
                                           const std::string &threadId,
                                           int ownerPid) {
  auto warning_modal = ftxui::Renderer([threadId, ownerPid] {
    const auto &theme = ThemeManager::instance().getCurrentTheme();
    return FlatModalPanel(
        theme, "Thread Locked",
        ModalSection(
            theme,
            ftxui::vbox(
                {ftxui::text("WARNING") | ftxui::bold |
                     ftxui::color(theme.status_bar.error.normal.fg) |
                     ftxui::center,
                 ftxui::text(""),
                 ftxui::text("Thread " + threadId +
                             " is currently locked by PID " +
                             std::to_string(ownerPid) + ".") |
                     ftxui::color(theme.modals.fg),
                 ftxui::text("Press ESC to dismiss and enter Welcome screen.") |
                     ftxui::color(theme.base.dim)}),
            theme.modals.bg),
        62, 16, theme.status_bar.error.normal.fg);
  });

  return ftxui::CatchEvent(warning_modal, [&state](ftxui::Event event) {
    if (event == ftxui::Event::Escape) {
      state.popModal();
      state.setViewMode(TuiState::ViewMode::Welcome);
      return true;
    }
    return false;
  });
}

} // namespace firmius::tui

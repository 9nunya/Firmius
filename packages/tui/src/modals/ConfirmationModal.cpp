#include "modals/ConfirmationModal.hpp"
#include "TUIState.hpp"
#include "ThemeManager.hpp"
#include "modals/ModalLayout.hpp"
#include <ftxui/component/component.hpp>
#include <ftxui/dom/elements.hpp>

namespace firmius::tui {

ConfirmationModal::ConfirmationModal(std::string title, std::string message,
                                     std::function<void()> onConfirm,
                                     std::function<void()> onCancel)
    : title_(std::move(title)), message_(std::move(message)),
      onConfirm_(std::move(onConfirm)), onCancel_(std::move(onCancel)) {}

ftxui::Component ConfirmationModal::create(TuiState &state) {
  auto title = title_;
  auto message = message_;
  auto onConfirm = onConfirm_;
  auto onCancel = onCancel_;
  auto focusTarget = ftxui::Button("", [] {});

  auto component = ftxui::Renderer(focusTarget, [title, message]() {
    const auto &theme = ThemeManager::instance().getCurrentTheme();
    return FlatModalPanel(
        theme, title,
        ModalSection(
            theme,
            ftxui::vbox(
                {ftxui::paragraph(message) | ftxui::center |
                     ftxui::color(theme.modals.fg),
                 ftxui::text(""),
                 ftxui::hbox({
                     ftxui::text(" [Y]es ") | ftxui::bold |
                         ftxui::color(theme.modals.highlight_fg),
                     ftxui::text("   "),
                     ftxui::text(" [N]o ") | ftxui::bold |
                         ftxui::color(theme.status_bar.error.normal.fg),
                 }) | ftxui::center,
                 ftxui::text(""),
                 ftxui::text(" (Press Y/Enter for Yes, N/Esc for No) ") |
                     ftxui::color(theme.base.dim) | ftxui::center}),
            theme.modals.bg),
        56, 16) |
            ftxui::focus;
  });

  return ftxui::CatchEvent(component,
                             [onConfirm, onCancel, &state](ftxui::Event event) {
                               if (event == ftxui::Event::Character('y') ||
                                   event == ftxui::Event::Character('Y') ||
                                   event == ftxui::Event::Return) {
                                 state.deferUiMutation([&state, onConfirm]() {
                                   state.popModalImmediate();
                                   if (onConfirm) {
                                     onConfirm();
                                   }
                                 });
                                 state.postEvent(ftxui::Event::Custom);
                                 return true;
                               }
                               if (event == ftxui::Event::Character('n') ||
                                   event == ftxui::Event::Character('N') ||
                                   event == ftxui::Event::Escape) {
                                 state.deferUiMutation([&state, onCancel]() {
                                   state.popModalImmediate();
                                   if (onCancel) {
                                     onCancel();
                                   }
                                 });
                                 state.postEvent(ftxui::Event::Custom);
                                 return true;
                               }
                               return false;
                             });

}

} // namespace firmius::tui

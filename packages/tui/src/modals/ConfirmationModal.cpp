#include "modals/ConfirmationModal.hpp"
#include "TUIState.hpp"
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

  auto component = ftxui::Renderer([title, message]() {
    return ftxui::window(
               ftxui::text(" " + title + " ") | ftxui::bold |
                   ftxui::color(ftxui::Color::Yellow),
               ftxui::vbox(
                   {ftxui::text(message) | ftxui::center, ftxui::text(""),
                    ftxui::hbox({
                        ftxui::text(" [Y]es ") | ftxui::bold |
                            ftxui::color(ftxui::Color::Green),
                        ftxui::text("   "),
                        ftxui::text(" [N]o ") | ftxui::bold |
                            ftxui::color(ftxui::Color::Red),
                    }) | ftxui::center,
                    ftxui::text(""),
                    ftxui::text(" (Press Y/Enter for Yes, N/Esc for No) ") |
                        ftxui::dim | ftxui::center})) |
           ftxui::clear_under | ftxui::center;
  });

  return ftxui::CatchEvent(component,
                           [onConfirm, onCancel, &state](ftxui::Event event) {
                             if (event == ftxui::Event::Character('y') ||
                                 event == ftxui::Event::Character('Y') ||
                                 event == ftxui::Event::Return) {
                               state.popModalImmediate();
                               if (onConfirm)
                                 onConfirm();
                               return true;
                             }
                             if (event == ftxui::Event::Character('n') ||
                                 event == ftxui::Event::Character('N') ||
                                 event == ftxui::Event::Escape) {
                               state.popModalImmediate();
                               if (onCancel)
                                 onCancel();
                               return true;
                             }
                             return false;
                           });
}

} // namespace firmius::tui

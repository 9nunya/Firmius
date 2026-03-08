#include "modals/ThreadPickerModal.hpp"
#include "TUIState.hpp"
#include "harness/Harness.hpp"
#include <ftxui/component/component.hpp>
#include <ftxui/dom/elements.hpp>
#include <vector>

namespace firmius::tui {

ftxui::Component ThreadPickerModal::create(TuiState &state) {
  auto &h = firmius::core::Harness::instance();
  auto selected = std::make_shared<int>(0);
  auto entries = std::make_shared<std::vector<std::string>>();
  auto threads_ptr =
      std::make_shared<std::vector<firmius::shared::ThreadMetadata>>();

  auto refresh_threads = [entries, threads_ptr, &h] {
    *threads_ptr = h.listThreads();
    entries->clear();
    for (const auto &t : *threads_ptr) {
      entries->push_back(t.title);
    }
  };

  // Initial load
  refresh_threads();

  auto menu = ftxui::Menu(entries.get(), selected.get());

  auto modal_renderer = ftxui::Renderer(menu, [menu, entries, selected] {
    return ftxui::window(
        ftxui::text(" Select Thread ") | ftxui::bold |
            ftxui::color(ftxui::Color::Cyan),
        ftxui::vbox({menu->Render() | ftxui::vscroll_indicator | ftxui::yframe |
                         ftxui::size(ftxui::HEIGHT, ftxui::LESS_THAN, 15),
                     ftxui::text(""),
                     ftxui::text("Press Enter to switch, ESC to cancel.") |
                         ftxui::dim}));
  });


  return ftxui::CatchEvent(
      modal_renderer, [threads_ptr, selected, entries, &state, &h](ftxui::Event event) {
        if (event == ftxui::Event::Return) {
          if (*selected >= 0 && *selected < (int)threads_ptr->size()) {
            if (h.switchThread((*threads_ptr)[*selected].threadId)) {
              state.popModal();
            }
          } else {
            state.popModal();
          }
          return true;
        }
        if (event == ftxui::Event::Escape) {
          state.popModal();
          return true;
        }
        return false;
      });
}

} // namespace firmius::tui

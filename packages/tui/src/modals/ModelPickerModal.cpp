#include "modals/ModelPickerModal.hpp"
#include "TUIState.hpp"
#include "harness/Harness.hpp"
#include <algorithm>
#include <ftxui/component/component.hpp>
#include <ftxui/dom/elements.hpp>
#include <string>
#include <vector>

namespace firmius::tui {

ftxui::Component ModelPickerModal::create(TuiState &state) {
  auto models = firmius::core::Harness::instance().listAllModels();

  auto entries = std::make_shared<std::vector<std::string>>();
  auto filtered_indices = std::make_shared<std::vector<int>>();
  auto filter_text = std::make_shared<std::string>("");
  auto selected = std::make_shared<int>(0);

  // Build initial entries
  for (int i = 0; i < (int)models.size(); ++i) {
    entries->push_back(models[i].provider + "/" + models[i].id);
    filtered_indices->push_back(i);
  }

  auto rebuild_filtered = [entries, filtered_indices, models, filter_text,
                           selected]() {
    filtered_indices->clear();
    std::string q = *filter_text;
    std::transform(q.begin(), q.end(), q.begin(), ::tolower);
    for (int i = 0; i < (int)models.size(); ++i) {
      std::string label = (*entries)[i];
      std::string lower_label = label;
      std::transform(lower_label.begin(), lower_label.end(),
                     lower_label.begin(), ::tolower);
      if (q.empty() || lower_label.find(q) != std::string::npos) {
        filtered_indices->push_back(i);
      }
    }
    if (*selected >= (int)filtered_indices->size()) {
      *selected =
          filtered_indices->empty() ? 0 : (int)filtered_indices->size() - 1;
    }
  };

  auto component = ftxui::Renderer([entries, filtered_indices, filter_text,
                                    selected, rebuild_filtered]() {
    rebuild_filtered();
    ftxui::Elements rows;
    for (int i = 0; i < (int)filtered_indices->size() && i < 20; ++i) {
      int idx = (*filtered_indices)[i];
      auto label = ftxui::text((*entries)[idx]);
      if (i == *selected) {
        label = label | ftxui::inverted;
      }
      rows.push_back(label);
    }
    if (rows.empty()) {
      rows.push_back(ftxui::text("No matching models") | ftxui::dim);
    }

    return ftxui::window(
               ftxui::text(" Select Model ") | ftxui::bold |
                   ftxui::color(ftxui::Color::Cyan),
               ftxui::vbox({
                   ftxui::hbox({ftxui::text("Filter: "),
                                ftxui::text(*filter_text) | ftxui::underlined}),
                   ftxui::separator(),
                   ftxui::vbox(rows) | ftxui::vscroll_indicator |
                       ftxui::yframe |
                       ftxui::size(ftxui::HEIGHT, ftxui::LESS_THAN, 15),
                   ftxui::text(""),
                   ftxui::text("↑↓ navigate, Enter select, ESC cancel, type to "
                               "filter") |
                       ftxui::dim,
               })) |
           ftxui::clear_under | ftxui::center;
  });

  return ftxui::CatchEvent(component, [models, entries, filtered_indices,
                                       filter_text, selected, rebuild_filtered,
                                       &state](ftxui::Event event) {
    if (event == ftxui::Event::Escape) {
      state.popModal();
      return true;
    }
    if (event == ftxui::Event::Return) {
      rebuild_filtered();
      if (!filtered_indices->empty() &&
          *selected < (int)filtered_indices->size()) {
        int idx = (*filtered_indices)[*selected];
        const auto &m = models[idx];
        firmius::core::Harness::instance().switchModel(m.provider, m.id);
      }
      state.popModal();
      return true;
    }
    if (event == ftxui::Event::ArrowUp) {
      if (*selected > 0)
        (*selected)--;
      return true;
    }
    if (event == ftxui::Event::ArrowDown) {
      rebuild_filtered();
      if (*selected < (int)filtered_indices->size() - 1)
        (*selected)++;
      return true;
    }
    if (event == ftxui::Event::Backspace) {
      if (!filter_text->empty()) {
        filter_text->pop_back();
        *selected = 0;
      }
      return true;
    }
    if (event.is_character()) {
      *filter_text += event.character();
      *selected = 0;
      return true;
    }
    return false;
  });
}

} // namespace firmius::tui

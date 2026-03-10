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
  auto models = std::make_shared<std::vector<firmius::core::ModelInfo>>();
  auto entries = std::make_shared<std::vector<std::string>>();
  auto filtered_indices = std::make_shared<std::vector<int>>();
  auto filter_text = std::make_shared<std::string>("");
  auto selected = std::make_shared<int>(0);
  auto isLoading = std::make_shared<bool>(true);

  auto display_entries = std::make_shared<std::vector<std::string>>();

  auto rebuild_filtered = [entries, filtered_indices, models, filter_text,
                           selected, display_entries]() {
    filtered_indices->clear();
    display_entries->clear();
    std::string q = *filter_text;
    std::transform(q.begin(), q.end(), q.begin(), ::tolower);

    // Split query into tokens for "AND" search
    std::vector<std::string> tokens;
    size_t start = 0, end = 0;
    while ((end = q.find(' ', start)) != std::string::npos) {
      if (end != start)
        tokens.push_back(q.substr(start, end - start));
      start = end + 1;
    }
    if (start < q.size())
      tokens.push_back(q.substr(start));

    for (int i = 0; i < (int)models->size(); ++i) {
      std::string lower_label = (*entries)[i];
      std::transform(lower_label.begin(), lower_label.end(),
                     lower_label.begin(), ::tolower);

      bool all_tokens_match = true;
      for (const auto &token : tokens) {
        if (lower_label.find(token) == std::string::npos) {
          all_tokens_match = false;
          break;
        }
      }

      if (all_tokens_match) {
        filtered_indices->push_back(i);
        display_entries->push_back((*entries)[i]);
      }
    }
    if (*selected >= (int)filtered_indices->size()) {
      *selected =
          filtered_indices->empty() ? 0 : (int)filtered_indices->size() - 1;
    }
  };

  auto refresh = [models, entries, filtered_indices, isLoading, selected,
                  rebuild_filtered]() {
    auto &h = firmius::core::Harness::instance();
    auto fetched = h.listAllModels();
    *models = std::move(fetched);
    entries->clear();
    for (int i = 0; i < (int)models->size(); ++i) {
      entries->push_back((*models)[i].provider + "/" + (*models)[i].id);
    }
    *isLoading = !h.isModelsLoaded();
    rebuild_filtered();
  };

  // Initial load
  refresh();

  // Subscribe to refreshes safely
  auto needs_refresh = std::make_shared<std::atomic<bool>>(false);
  int subId = firmius::core::Harness::instance().subscribe(
      [needs_refresh, &state](const firmius::shared::AppEvent &event) {
        if (std::holds_alternative<firmius::shared::ModelsRefreshed>(event)) {
          *needs_refresh = true;
          state.postEvent(ftxui::Event::Custom);
        }
      });

  auto menu = ftxui::Menu(display_entries.get(), selected.get());

  auto component = ftxui::Renderer(menu, [entries, filtered_indices,
                                          filter_text, selected,
                                          rebuild_filtered, isLoading,
                                          display_entries, menu, needs_refresh,
                                          refresh]() {
    if (*needs_refresh) {
      *needs_refresh = false;
      refresh();
    }
    if (*isLoading) {
      return ftxui::window(
                 ftxui::text(" Select Model ") | ftxui::bold |
                     ftxui::color(ftxui::Color::Cyan),
                 ftxui::vbox(
                     {ftxui::text("Loading models...") | ftxui::center,
                      ftxui::text("") |
                          ftxui::size(ftxui::HEIGHT, ftxui::EQUAL, 5)})) |
             ftxui::clear_under | ftxui::center;
    }

    rebuild_filtered();

    if (display_entries->empty()) {
      return ftxui::window(
                 ftxui::text(" Select Model ") | ftxui::bold |
                     ftxui::color(ftxui::Color::Cyan),
                 ftxui::vbox({ftxui::hbox({ftxui::text("Filter: "),
                                           ftxui::text(*filter_text) |
                                               ftxui::underlined}),
                              ftxui::separator(),
                              ftxui::text("No matching models") | ftxui::dim |
                                  ftxui::center,
                              ftxui::text(""),
                              ftxui::text(" ESC cancel, type to filter ") |
                                  ftxui::dim | ftxui::center})) |
             ftxui::clear_under | ftxui::center;
    }

    return ftxui::window(
               ftxui::text(" Select Model ") | ftxui::bold |
                   ftxui::color(ftxui::Color::Cyan),
               ftxui::vbox({
                   ftxui::hbox({ftxui::text("Filter: "),
                                ftxui::text(*filter_text) | ftxui::underlined}),
                   ftxui::separator(),
                   menu->Render() | ftxui::vscroll_indicator | ftxui::frame |
                       ftxui::size(ftxui::HEIGHT, ftxui::LESS_THAN, 15),
                   ftxui::text(""),
                   ftxui::text("↑↓ navigate, Enter select, ESC cancel, "
                               "type/mouse click to "
                               "filter/select") |
                       ftxui::dim | ftxui::center,
               })) |
           ftxui::clear_under | ftxui::center;
  });

  return ftxui::CatchEvent(component, [models, entries, filtered_indices,
                                       filter_text, selected, rebuild_filtered,
                                       isLoading, subId, menu,
                                       &state](ftxui::Event event) {
    if (*isLoading) {
      if (event == ftxui::Event::Escape) {
        firmius::core::Harness::instance().unsubscribe(subId);
        state.popModalImmediate();
        return true;
      }
      return false;
    }

    if (event == ftxui::Event::Escape) {
      firmius::core::Harness::instance().unsubscribe(subId);
      state.popModalImmediate();
      return true;
    }

    if (event == ftxui::Event::Return) {
      rebuild_filtered();
      if (!filtered_indices->empty() &&
          *selected < (int)filtered_indices->size()) {
        int idx = (*filtered_indices)[*selected];
        const auto &m = (*models)[idx];
        firmius::core::Harness::instance().switchModel(m.provider, m.id);
      }
      firmius::core::Harness::instance().unsubscribe(subId);
      state.popModalImmediate();
      return true;
    }

    if (event == ftxui::Event::ArrowUp || event == ftxui::Event::ArrowDown) {
      return menu->OnEvent(event);
    }

    if (event.is_mouse()) {
      return menu->OnEvent(event);
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

#include "modals/ModelPickerModal.hpp"
#include "TUIState.hpp"
#include "ThemeManager.hpp"
#include "harness/Harness.hpp"
#include "modals/ModalLayout.hpp"
#include "utils/ModelPickerEntries.hpp"
#include <algorithm>
#include <ftxui/component/component.hpp>
#include <ftxui/dom/elements.hpp>
#include <string>
#include <vector>

namespace firmius::tui {

ftxui::Component ModelPickerModal::create(TuiState &state) {
  auto entries = std::make_shared<std::vector<ModelPickerEntry>>();
  auto filtered_indices = std::make_shared<std::vector<int>>();
  auto filter_text = std::make_shared<std::string>("");
  auto selected = std::make_shared<int>(0);
  auto isLoading = std::make_shared<bool>(true);

  auto display_entries = std::make_shared<std::vector<std::string>>();

  auto rebuild_filtered = [entries, filtered_indices, filter_text, selected,
                           display_entries]() {
    *filtered_indices = FilterModelPickerEntries(*entries, *filter_text);
    display_entries->clear();
    for (int index : *filtered_indices) {
      display_entries->push_back((*entries)[index].label);
    }

    if (*selected >= (int)filtered_indices->size()) {
      *selected =
          filtered_indices->empty() ? 0 : (int)filtered_indices->size() - 1;
    }
  };

  auto refresh = [entries, isLoading, rebuild_filtered]() {
    auto &h = firmius::core::Harness::instance();
    auto fetched = h.listAllModels();
    *entries = BuildModelPickerEntries(fetched, true);
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
    const auto &theme = ThemeManager::instance().getCurrentTheme();
    if (*needs_refresh) {
      *needs_refresh = false;
      refresh();
    }
    if (*isLoading) {
      return FlatModalPanel(
          theme, "Select Model",
          ModalSection(
              theme,
              ftxui::vbox({ftxui::text("Loading models...") | ftxui::center |
                               ftxui::color(theme.modals.fg),
                           ftxui::text("") |
                               ftxui::size(ftxui::HEIGHT, ftxui::EQUAL, 5)}),
              theme.modals.bg));
    }

    rebuild_filtered();

    if (display_entries->empty()) {
      return FlatModalPanel(
          theme, "Select Model",
          ModalSection(
              theme,
              ftxui::vbox({ftxui::hbox({ftxui::text("Filter: ") |
                                            ftxui::color(theme.modals.fg),
                                        ftxui::text(*filter_text) |
                                            ftxui::underlined |
                                            ftxui::color(theme.modals.fg)}),
                           ftxui::separatorLight() |
                               ftxui::color(theme.modals.border),
                           ftxui::text("No matching models") |
                               ftxui::color(theme.base.dim) | ftxui::center,
                           ftxui::text(""),
                           ftxui::text(" ESC cancel, type to filter ") |
                               ftxui::color(theme.base.dim) | ftxui::center}),
              theme.modals.bg));
    }

    return FlatModalPanel(
        theme, "Select Model",
        ModalSection(
            theme,
            ftxui::vbox({
                ftxui::hbox(
                    {ftxui::text("Filter: ") | ftxui::color(theme.modals.fg),
                     ftxui::text(*filter_text) | ftxui::underlined |
                         ftxui::color(theme.modals.fg)}),
                ftxui::separatorLight() | ftxui::color(theme.modals.border),
                menu->Render() | ftxui::vscroll_indicator | ftxui::frame |
                    ftxui::size(ftxui::HEIGHT, ftxui::LESS_THAN, 15),
                ftxui::text(""),
                ftxui::text("↑↓ navigate, Enter select, ESC cancel, "
                            "type/mouse click to filter/select") |
                    ftxui::color(theme.base.dim) | ftxui::center,
            }),
            theme.modals.bg));
  });

  return ftxui::CatchEvent(component, [entries, filtered_indices,
                                       filter_text, selected, rebuild_filtered,
                                       isLoading, subId, menu,
                                       &state](ftxui::Event event) {
    if (*isLoading) {
      if (event == ftxui::Event::Escape) {
        firmius::core::Harness::instance().unsubscribe(subId);
        state.popModal();
        return true;
      }
      return false;
    }

    if (event == ftxui::Event::Escape) {
      firmius::core::Harness::instance().unsubscribe(subId);
      state.popModal();
      return true;
    }

    if (event == ftxui::Event::Return) {
      rebuild_filtered();
      if (!filtered_indices->empty() &&
          *selected < (int)filtered_indices->size()) {
        int index = (*filtered_indices)[*selected];
        const auto &entry = (*entries)[index];
        firmius::core::Harness::instance().switchModel(
            entry.provider_id, entry.model_id, entry.variant_name);
      }
      firmius::core::Harness::instance().unsubscribe(subId);
      state.popModal();
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

#include "modals/ModelPickerModal.hpp"

#include "NotificationManager.hpp"
#include "TUIState.hpp"
#include "ThemeManager.hpp"
#include "components/ScrollableBox.hpp"
#include "harness/Harness.hpp"
#include "modals/ModalLayout.hpp"
#include "utils/ModelPickerEntries.hpp"
#include <algorithm>
#include <atomic>
#include <ftxui/component/component.hpp>
#include <ftxui/component/mouse.hpp>
#include <ftxui/dom/elements.hpp>
#include <string>
#include <vector>

namespace firmius::tui {

namespace {

std::string formatProviderList(std::vector<std::string> providerIds) {
  std::sort(providerIds.begin(), providerIds.end());
  if (providerIds.empty()) {
    return "";
  }

  std::string joined;
  for (size_t i = 0; i < providerIds.size(); ++i) {
    if (i > 0) {
      joined += ", ";
    }
    joined += providerIds[i];
  }
  return joined;
}

} // namespace

ftxui::Component ModelPickerModal::create(TuiState &state) {
  auto entries = std::make_shared<std::vector<ModelPickerEntry>>();
  auto filteredIndices = std::make_shared<std::vector<int>>();
  auto filterText = std::make_shared<std::string>("");
  auto selected = std::make_shared<int>(0);
  auto isLoading = std::make_shared<bool>(true);
  auto fetchingProviders = std::make_shared<std::vector<std::string>>();
  auto rowBoxes = std::make_shared<std::vector<ftxui::Box>>();
  auto visibleStart = std::make_shared<int>(0);
  auto visibleCount = std::make_shared<int>(24);
  auto modalActive = std::make_shared<std::atomic<bool>>(true);

  auto rebuildFiltered = [entries, filteredIndices, filterText, selected]() {
    *filteredIndices = FilterModelPickerEntries(*entries, *filterText);
    if (filteredIndices->empty()) {
      *selected = 0;
    } else {
      *selected = std::clamp(*selected, 0,
                             static_cast<int>(filteredIndices->size() - 1));
    }
  };

  auto refresh = [entries, isLoading, fetchingProviders, rebuildFiltered]() {
    auto &h = firmius::core::Harness::instance();
    *entries = BuildModelPickerEntries(h.cachedModelsSnapshot(), true);
    *isLoading = !h.isModelsLoaded();
    *fetchingProviders = h.listProvidersFetchingModels();
    rebuildFiltered();
  };

  refresh();
  state.runBackgroundTask([]() { firmius::core::Harness::instance().listAllModels(); });

  int subId = firmius::core::Harness::instance().subscribe(
      [entries, isLoading, fetchingProviders, rebuildFiltered, modalActive,
       &state](const firmius::shared::AppEvent &event) {
        if (std::holds_alternative<firmius::shared::ModelsRefreshed>(event) ||
            std::holds_alternative<firmius::shared::ProviderModelsFetchStarted>(
                event) ||
            std::holds_alternative<firmius::shared::ProviderModelsFetchFinished>(
                event) ||
            std::holds_alternative<firmius::shared::ModelDiscovered>(event)) {
          state.deferUiMutation([entries, isLoading, fetchingProviders,
                                 rebuildFiltered, modalActive]() {
            if (!modalActive->load(std::memory_order_relaxed)) {
              return;
            }
            auto &h = firmius::core::Harness::instance();
            *entries = BuildModelPickerEntries(h.cachedModelsSnapshot(), true);
            *isLoading = !h.isModelsLoaded();
            *fetchingProviders = h.listProvidersFetchingModels();
            rebuildFiltered();
          });
        }
      });

  auto listContent = ftxui::Renderer([entries, filteredIndices, selected,
                                      rowBoxes, visibleStart, visibleCount]() {
    const auto &theme = ThemeManager::instance().getCurrentTheme();
    const int total = static_cast<int>(filteredIndices->size());
    const int start = std::clamp(*visibleStart, 0, std::max(0, total - 1));
    const int count = std::max(1, *visibleCount);
    const int end = std::min(total, start + count);
    rowBoxes->assign(std::max(0, end - start), ftxui::Box{});

    if (filteredIndices->empty()) {
      return ftxui::vbox({
          ftxui::text("No matching models") | ftxui::center |
              ftxui::color(theme.base.dim),
          ftxui::text(""),
          ftxui::text("Try provider, model, variant, or ctx terms") |
              ftxui::center | ftxui::color(theme.base.dim),
      });
    }

    ftxui::Elements rows;
    rows.reserve(static_cast<std::size_t>(std::max(0, end - start) * 2 + 2));
    if (start > 0) {
      rows.push_back(ftxui::text("  ... " + std::to_string(start) +
                                 " more above") |
                     ftxui::color(theme.base.dim));
      rows.push_back(ftxui::text(""));
    }
    for (int rowIndex = start; rowIndex < end; ++rowIndex) {
      const int visibleIndex = rowIndex - start;
      const auto &entry = (*entries)[(*filteredIndices)[rowIndex]];
      auto row = ftxui::vbox({
                     ftxui::hbox({
                         ftxui::text("  " + entry.title) | ftxui::bold |
                             ftxui::color(theme.modals.fg),
                         ftxui::text("  " + entry.provider_label) |
                             ftxui::color(theme.base.dim),
                         ftxui::filler(),
                     }),
                     ftxui::text("  " + entry.meta_label + "  ") |
                         ftxui::color(theme.base.dim),
                 }) |
                 ftxui::reflect(rowBoxes->at(visibleIndex));

      if (rowIndex == *selected) {
        row = row | ftxui::bgcolor(theme.modals.highlight_bg);
      }

      rows.push_back(row);
      rows.push_back(ftxui::text(""));
    }
    if (end < total) {
      rows.push_back(ftxui::text("  ... " + std::to_string(total - end) +
                                 " more below") |
                     ftxui::color(theme.base.dim));
    }
    return ftxui::vbox(std::move(rows));
  });

  auto scrollable =
      ScrollableBox(listContent, {.startAtBottom = false, .overlayScrollbar = true});
  scrollable->RequestScrollToTop();

  auto component = ftxui::Renderer(scrollable, [entries, filteredIndices,
                                                filterText, selected,
                                                rebuildFiltered, isLoading,
                                                fetchingProviders, scrollable,
                                                visibleStart, visibleCount]() {
    const auto &theme = ThemeManager::instance().getCurrentTheme();
    const auto terminal = ftxui::Terminal::Size();
    const int panelWidth = std::clamp(std::max(0, terminal.dimx - 8), 60, 96);
    const int panelHeight = std::clamp(std::max(0, terminal.dimy - 6), 18, 28);
    const int listHeight = std::max(8, panelHeight - 11);
    const int windowRows = std::max(8, listHeight / 2);
    *visibleCount = windowRows;
    if (!filteredIndices->empty()) {
      const int maxStart =
          std::max(0, static_cast<int>(filteredIndices->size()) - windowRows);
      *visibleStart = std::clamp(*selected - windowRows / 2, 0, maxStart);
    } else {
      *visibleStart = 0;
    }

    rebuildFiltered();

    const std::string fetchingLabel = formatProviderList(*fetchingProviders);

    if (*isLoading && entries->empty()) {
      ftxui::Elements loadingRows = {
          ftxui::text("Scanning providers for models...") | ftxui::center |
              ftxui::color(theme.modals.fg),
          ftxui::text(""),
          ftxui::text("The picker will populate as providers respond.") |
              ftxui::center | ftxui::color(theme.base.dim),
      };
      if (!fetchingLabel.empty()) {
        loadingRows.push_back(ftxui::text(""));
        loadingRows.push_back(
            ftxui::text("Fetching: " + fetchingLabel) | ftxui::center |
            ftxui::color(theme.base.dim));
      }
      loadingRows.push_back(
          ftxui::text("") | ftxui::size(ftxui::HEIGHT, ftxui::EQUAL, 4));
      return FlatModalPanel(
          theme, "Select Model",
          ModalSection(theme, ftxui::vbox(std::move(loadingRows)),
                       theme.modals.bg),
          panelWidth, 16);
    }

    auto statusBadge =
        ftxui::text(*isLoading ? " scanning " : " ready ") | ftxui::bold |
        ftxui::color(theme.modals.highlight_fg) |
        ftxui::bgcolor(theme.modals.highlight_bg);

    auto body = ftxui::vbox({
        ftxui::hbox({
            ftxui::text("Filter: ") | ftxui::color(theme.modals.fg),
            ftxui::text(*filterText) | ftxui::underlined |
                ftxui::color(theme.modals.fg),
            ftxui::filler(),
            statusBadge,
        }),
        ftxui::text(""),
        *isLoading && !fetchingLabel.empty()
            ? (ftxui::text("Fetching providers: " + fetchingLabel) |
               ftxui::color(theme.base.dim))
            : ftxui::text(""),
        ftxui::hbox({
            ftxui::text(" " + std::to_string(filteredIndices->size()) + " matches ") |
                ftxui::color(theme.base.dim),
            ftxui::filler(),
            ftxui::text("provider  model  variant  ctx  vision") |
                ftxui::color(theme.base.dim),
        }),
        ftxui::separatorLight() | ftxui::color(theme.modals.border),
        scrollable->Render() | ftxui::xflex | ftxui::size(ftxui::HEIGHT, ftxui::EQUAL,
                                                          listHeight),
        ftxui::text(""),
        ftxui::text("Type to fuzzy-search prettified and raw names. Enter selects.") |
            ftxui::color(theme.base.dim) | ftxui::center,
    });

    return FlatModalPanel(theme, "Select Model",
                          ModalSection(theme, std::move(body), theme.modals.bg),
                          panelWidth, panelHeight);
  });

  return ftxui::CatchEvent(component, [entries, filteredIndices, filterText,
                                       selected, rebuildFiltered, isLoading,
                                       subId, scrollable, rowBoxes, modalActive,
                                       visibleStart, &state](ftxui::Event event) {
    const auto closeModal = [&]() {
      modalActive->store(false, std::memory_order_relaxed);
      firmius::core::Harness::instance().unsubscribe(subId);
      state.popModal();
    };

    if (event == ftxui::Event::Escape) {
      closeModal();
      return true;
    }

    if (event == ftxui::Event::Return) {
      rebuildFiltered();
      if (!filteredIndices->empty() &&
          *selected < static_cast<int>(filteredIndices->size())) {
        const auto &entry = (*entries)[(*filteredIndices)[*selected]];
        try {
          firmius::core::Harness::instance().switchModel(
              entry.provider_id, entry.model_id, entry.variant_name);
        } catch (const std::exception &ex) {
          NotificationManager::instance().notifyError("Model", ex.what(),
                                                      false);
        }
      }
      closeModal();
      return true;
    }

    if (event == ftxui::Event::ArrowUp || event == ftxui::Event::PageUp ||
        event == ftxui::Event::Home) {
      if (!filteredIndices->empty()) {
        if (event == ftxui::Event::Home) {
          *selected = 0;
          scrollable->RequestScrollToTop();
        } else if (event == ftxui::Event::PageUp) {
          *selected = std::max(0, *selected - 6);
        } else {
          *selected = std::max(0, *selected - 1);
        }
      }
      scrollable->OnEvent(event);
      return true;
    }

    if (event == ftxui::Event::ArrowDown || event == ftxui::Event::PageDown ||
        event == ftxui::Event::End) {
      if (!filteredIndices->empty()) {
        const int last = static_cast<int>(filteredIndices->size() - 1);
        if (event == ftxui::Event::End) {
          *selected = last;
        } else if (event == ftxui::Event::PageDown) {
          *selected = std::min(last, *selected + 6);
        } else {
          *selected = std::min(last, *selected + 1);
        }
      }
      scrollable->OnEvent(event);
      return true;
    }

    if (event.is_mouse()) {
      const auto mouse = event.mouse();
      const bool isDragRelatedLeftMouse =
          mouse.button == ftxui::Mouse::Left ||
          mouse.motion == ftxui::Mouse::Moved ||
          mouse.motion == ftxui::Mouse::Released;
      if (isDragRelatedLeftMouse && scrollable->OnEvent(event)) {
        return true;
      }
      if (mouse.button == ftxui::Mouse::WheelUp && *selected > 0) {
        --(*selected);
      }
      if (mouse.button == ftxui::Mouse::WheelDown && !filteredIndices->empty()) {
        *selected = std::min(*selected + 1,
                             static_cast<int>(filteredIndices->size() - 1));
      }
      if (mouse.button == ftxui::Mouse::Left &&
          mouse.motion == ftxui::Mouse::Pressed) {
        for (int rowIndex = 0; rowIndex < static_cast<int>(rowBoxes->size());
             ++rowIndex) {
          if (rowBoxes->at(rowIndex).Contain(mouse.x, mouse.y)) {
            *selected = *visibleStart + rowIndex;
            return true;
          }
        }
      }
      if (isDragRelatedLeftMouse) {
        return false;
      }
      return scrollable->OnEvent(event);
    }

        if (event == ftxui::Event::Backspace) {
      if (!filterText->empty()) {
        filterText->pop_back();
        *selected = 0;
        scrollable->RequestScrollToTop();
      }
      return true;
    }
    if (event.is_character()) {
      *filterText += event.character();
      *selected = 0;
      scrollable->RequestScrollToTop();
      return true;
    }
    return false;
  });
}

} // namespace firmius::tui

#include "modals/CommandPaletteModal.hpp"

#include "ThemeManager.hpp"
#include "commands/CommandManager.hpp"
#include "models/TUIStore.hpp"
#include "modals/ModalLayout.hpp"

#include <algorithm>
#include <cctype>
#include <ftxui/component/component.hpp>
#include <ftxui/dom/elements.hpp>
#include <memory>
#include <string>
#include <vector>

namespace firmius::tui {
namespace {

std::string lowerCopy(std::string text) {
  std::transform(text.begin(), text.end(), text.begin(), [](unsigned char ch) {
    return static_cast<char>(std::tolower(ch));
  });
  return text;
}

struct PaletteRow {
  CommandPaletteEntry entry;
  std::string preview;
};

} // namespace

ftxui::Component CommandPaletteModal::create(TuiState &state) {
  auto query = std::make_shared<std::string>();
  auto selected = std::make_shared<int>(0);
  auto rows = std::make_shared<std::vector<PaletteRow>>();

  auto rebuild = [query, selected, rows]() {
    rows->clear();
    const std::string q = lowerCopy(*query);
    for (const auto &entry : CommandManager::instance().listCommands()) {
      const std::string haystack = lowerCopy(entry.name + " " + entry.description);
      if (!q.empty() && haystack.find(q) == std::string::npos) {
        continue;
      }
      rows->push_back({entry, entry.completion});
    }
    std::sort(rows->begin(), rows->end(), [](const PaletteRow &a, const PaletteRow &b) {
      if (a.entry.is_workflow != b.entry.is_workflow) {
        return !a.entry.is_workflow && b.entry.is_workflow;
      }
      return a.entry.name < b.entry.name;
    });
    if (rows->empty()) {
      *selected = 0;
    } else {
      *selected = std::clamp(*selected, 0, static_cast<int>(rows->size()) - 1);
    }
  };

  rebuild();

  auto renderer = ftxui::Renderer([query, selected, rows]() {
    const auto &theme = ThemeManager::instance().getCurrentTheme();

    ftxui::Elements row_elements;
    if (rows->empty()) {
      row_elements.push_back(ftxui::text("No matching commands") |
                             ftxui::color(theme.base.dim));
    } else {
      for (size_t i = 0; i < rows->size(); ++i) {
        const auto &row = rows->at(i);
        const bool active = static_cast<int>(i) == *selected;
        auto line = ftxui::hbox({
                        ftxui::text(active ? "> " : "  ") |
                            ftxui::color(active ? theme.modals.highlight_fg
                                                : theme.modals.fg),
                        ftxui::text("/" + row.entry.name) | ftxui::bold |
                            ftxui::color(active ? theme.modals.highlight_fg
                                                : theme.modals.fg),
                        ftxui::text("  " + row.entry.description) |
                            ftxui::color(active ? theme.modals.highlight_fg
                                                : theme.base.dim) |
                            ftxui::flex,
                    });
        if (active) {
          line = line | ftxui::bgcolor(theme.modals.highlight_bg);
        }
        row_elements.push_back(line);

        for (const auto &hint : row.entry.binding_hints) {
          row_elements.push_back(
              ftxui::text("      " + hint.label + " — " + hint.description) |
              ftxui::color(theme.base.dim));
        }
      }
    }

    const std::string preview =
        rows->empty() ? std::string("/") : rows->at(*selected).preview;

    auto body = ftxui::vbox({
        ftxui::text("Search") | ftxui::bold |
            ftxui::color(theme.modals.title),
        ftxui::text(query->empty() ? "(all commands)" : *query) |
            ftxui::color(theme.modals.fg),
        ftxui::separatorLight() | ftxui::color(theme.modals.border),
        ftxui::vbox(std::move(row_elements)) | ftxui::yframe | ftxui::xflex |
            ftxui::size(ftxui::HEIGHT, ftxui::LESS_THAN, 18),
        ftxui::separatorLight() | ftxui::color(theme.modals.border),
        ftxui::text("Launch preview: " + preview) |
            ftxui::color(theme.base.dim),
        ftxui::text("Type to filter. Enter loads the command into input. Esc closes.") |
            ftxui::color(theme.base.dim),
    });

    return FlatModalPanel(theme, "Command Palette",
                          ModalSection(theme, std::move(body), theme.modals.bg),
                          96, 26);
  });

  return ftxui::CatchEvent(renderer, [query, selected, rows, rebuild, &state](ftxui::Event event) {
    if (event == ftxui::Event::Escape) {
      state.popModal();
      return true;
    }
    if (event == ftxui::Event::ArrowUp) {
      if (!rows->empty()) {
        *selected = (*selected + static_cast<int>(rows->size()) - 1) %
                    static_cast<int>(rows->size());
      }
      return true;
    }
    if (event == ftxui::Event::ArrowDown) {
      if (!rows->empty()) {
        *selected = (*selected + 1) % static_cast<int>(rows->size());
      }
      return true;
    }
    if (event == ftxui::Event::Backspace) {
      if (!query->empty()) {
        query->pop_back();
        rebuild();
      }
      return true;
    }
    if (event == ftxui::Event::Return) {
      auto &store = TUIStore::instance();
      if (!rows->empty() && store.input_model && store.input_model->buffer &&
          store.input_model->cursor) {
        *store.input_model->buffer = rows->at(*selected).preview;
        *store.input_model->cursor = static_cast<int>(store.input_model->buffer->size());
      }
      state.popModal();
      return true;
    }

    const std::string raw = event.input();
    if (raw.size() == 1 && std::isprint(static_cast<unsigned char>(raw[0])) != 0) {
      query->append(raw);
      rebuild();
      return true;
    }
    return false;
  });
}

} // namespace firmius::tui

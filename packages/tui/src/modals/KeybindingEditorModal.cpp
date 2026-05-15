#include "modals/KeybindingEditorModal.hpp"

#include "TUIHotkeys.hpp"
#include "TUIState.hpp"
#include "ThemeManager.hpp"
#include "modals/ModalLayout.hpp"

#include <algorithm>
#include <ftxui/component/component.hpp>
#include <ftxui/dom/elements.hpp>
#include <memory>
#include <string>
#include <vector>

namespace firmius::tui {
namespace {

struct EditableBindingRow {
  HotkeyAction action;
  std::string category;
  std::string action_name;
  std::string description;
  std::string label;
};

std::vector<EditableBindingRow> buildEditableRows() {
  std::vector<EditableBindingRow> rows;
  const auto bindings = LoadHotkeyConfig().bindings;
  for (const auto &spec : HotkeyBindingSpecs()) {
    std::string label = spec.default_label;
    auto it = std::find_if(bindings.begin(), bindings.end(), [&](const HotkeyBinding &binding) {
      return binding.action == spec.action;
    });
    if (it != bindings.end()) {
      label = it->label;
    }
    rows.push_back({spec.action,
                    spec.category,
                    HotkeyActionName(spec.action),
                    HotkeyActionDescription(spec.action),
                    label});
  }
  return rows;
}

std::vector<HotkeyBinding> collectBindings(
    const std::vector<EditableBindingRow> &rows) {
  std::vector<HotkeyBinding> bindings;
  bindings.reserve(rows.size());
  for (const auto &row : rows) {
    bindings.push_back({row.action, row.label});
  }
  return bindings;
}

std::string conflictSummary(const std::vector<HotkeyConflict> &conflicts) {
  if (conflicts.empty()) {
    return "No duplicate conflicts.";
  }
  std::string text = "Conflicts: ";
  for (size_t i = 0; i < conflicts.size(); ++i) {
    if (i > 0) {
      text += " | ";
    }
    text += conflicts[i].label + " → ";
    for (size_t j = 0; j < conflicts[i].actions.size(); ++j) {
      if (j > 0) {
        text += ", ";
      }
      text += HotkeyActionName(conflicts[i].actions[j]);
    }
  }
  return text;
}

} // namespace

ftxui::Component KeybindingEditorModal::create(TuiState &state) {
  auto rows = std::make_shared<std::vector<EditableBindingRow>>(buildEditableRows());
  auto selected = std::make_shared<int>(0);
  auto editing = std::make_shared<bool>(false);
  auto draft = std::make_shared<std::string>();
  auto message = std::make_shared<std::string>("Edit a binding, then save or reload.");
  auto warnings = std::make_shared<std::vector<std::string>>();
  auto conflicts = std::make_shared<std::vector<HotkeyConflict>>();

  auto recompute = [rows, conflicts]() {
    *conflicts = FindHotkeyConflicts(collectBindings(*rows));
  };
  recompute();

  auto loadFromDisk = [rows, selected, editing, draft, warnings, conflicts, message]() {
    *rows = buildEditableRows();
    *selected = std::clamp(*selected, 0, static_cast<int>(rows->size()) - 1);
    *editing = false;
    draft->clear();
    *warnings = LoadHotkeyConfig().warnings;
    *conflicts = FindHotkeyConflicts(collectBindings(*rows));
    *message = warnings->empty() ? "Reloaded bindings from disk."
                                 : warnings->front();
  };

  auto save = [rows, warnings, conflicts, message]() {
    *conflicts = FindHotkeyConflicts(collectBindings(*rows));
    if (!conflicts->empty()) {
      *message = "Resolve duplicate conflicts before saving.";
      return;
    }
    if (SaveHotkeyConfig(collectBindings(*rows), warnings.get())) {
      ReloadHotkeyConfig(warnings.get());
      *message = warnings->empty() ? "Saved hotkeys." : warnings->front();
    } else {
      *message = warnings->empty() ? "Failed to save hotkeys." : warnings->front();
    }
  };

  auto resetDefaults = [rows, selected, editing, draft, warnings, conflicts, message]() {
    auto bindings = ResetHotkeyBindingsToDefaults(warnings.get());
    (void)bindings;
    *rows = buildEditableRows();
    *selected = std::clamp(*selected, 0, static_cast<int>(rows->size()) - 1);
    *editing = false;
    draft->clear();
    *conflicts = FindHotkeyConflicts(collectBindings(*rows));
    *message = warnings->empty() ? "Reset to defaults." : warnings->front();
  };

  auto renderer = ftxui::Renderer([rows, selected, editing, draft, message, warnings, conflicts]() {
    const auto &theme = ThemeManager::instance().getCurrentTheme();
    ftxui::Elements items;
    for (size_t i = 0; i < rows->size(); ++i) {
      const auto &row = rows->at(i);
      const bool active = static_cast<int>(i) == *selected;
      const std::string shown_label =
          active && *editing ? *draft : row.label;
      auto line = ftxui::hbox({
                      ftxui::text(active ? "> " : "  "),
                      ftxui::text(row.category + " / " + row.action_name) |
                          ftxui::bold |
                          ftxui::color(active ? theme.modals.highlight_fg
                                              : theme.modals.fg),
                      ftxui::filler(),
                      ftxui::text(shown_label) |
                          ftxui::color(active ? theme.modals.highlight_fg
                                              : theme.base.dim),
                  });
      if (active) {
        // Anchor the surrounding yframe scroll on the active row so ArrowDown
        // past the visible window pulls the list down instead of running off.
        line = line | ftxui::bgcolor(theme.modals.highlight_bg) | ftxui::focus;
      }
      items.push_back(line);
      auto desc = ftxui::text("      " + row.description) |
                  ftxui::color(theme.base.dim);
      if (active) {
        desc = desc | ftxui::focus;
      }
      items.push_back(desc);
    }

    std::string warning_text;
    if (!warnings->empty()) {
      warning_text = warnings->front();
    }
    auto body = ftxui::vbox({
        ftxui::vbox(std::move(items)) | ftxui::yframe |
            ftxui::size(ftxui::HEIGHT, ftxui::LESS_THAN, 16),
        ftxui::separatorLight() | ftxui::color(theme.modals.border),
        ftxui::text(conflictSummary(*conflicts)) |
            ftxui::color(conflicts->empty() ? theme.base.dim
                                            : theme.status_bar.error.normal.fg),
        ftxui::text(warning_text.empty() ? *message : warning_text) |
            ftxui::color(theme.base.dim),
        ftxui::text(*editing ? "Editing: type label, Backspace deletes, Enter applies."
                             : "Enter edits. S save. R reload. D defaults. Esc close.") |
            ftxui::color(theme.base.dim),
    });

    return FlatModalPanel(theme, "Keybinding Editor",
                          ModalSection(theme, std::move(body), theme.modals.bg),
                          110, 26);
  });

  return ftxui::CatchEvent(renderer, [rows, selected, editing, draft, warnings, conflicts,
                                      message, recompute, save, loadFromDisk,
                                      resetDefaults, &state](ftxui::Event event) {
    if (event == ftxui::Event::Escape) {
      if (*editing) {
        *editing = false;
        draft->clear();
        *message = "Edit cancelled.";
      } else {
        state.popModal();
      }
      return true;
    }
    if (!*editing && event == ftxui::Event::ArrowUp) {
      *selected = std::max(0, *selected - 1);
      return true;
    }
    if (!*editing && event == ftxui::Event::ArrowDown) {
      *selected = std::min(static_cast<int>(rows->size()) - 1, *selected + 1);
      return true;
    }
    if (!*editing && event == ftxui::Event::Character('s')) {
      save();
      return true;
    }
    if (!*editing && event == ftxui::Event::Character('r')) {
      loadFromDisk();
      return true;
    }
    if (!*editing && event == ftxui::Event::Character('d')) {
      resetDefaults();
      return true;
    }
    if (!*editing && event == ftxui::Event::Return) {
      *editing = true;
      *draft = rows->at(*selected).label;
      warnings->clear();
      *message = "Editing binding text.";
      return true;
    }
    if (*editing && event == ftxui::Event::Backspace) {
      if (!draft->empty()) {
        draft->pop_back();
      }
      return true;
    }
    if (*editing && event == ftxui::Event::Return) {
      rows->at(*selected).label = *draft;
      *editing = false;
      warnings->clear();
      recompute();
      if (!ParseHotkeyLabel(rows->at(*selected).label).has_value()) {
        warnings->push_back("Edited label is not recognized yet.");
      }
      *message = conflicts->empty() ? "Binding updated." : "Binding updated with conflicts.";
      return true;
    }
    if (*editing) {
      const std::string raw = event.input();
      if (raw.size() == 1 && std::isprint(static_cast<unsigned char>(raw[0])) != 0) {
        draft->append(raw);
        return true;
      }
    }
    return false;
  });
}

} // namespace firmius::tui

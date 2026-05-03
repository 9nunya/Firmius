#include "components/HelpOverlay.hpp"
#include "TUIState.hpp"
#include "TUIHotkeys.hpp"
#include "ThemeManager.hpp"
#include "commands/CommandManager.hpp"
#include "components/ScrollableBox.hpp"
#include "modals/ModalLayout.hpp"
#include <ftxui/component/component.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/terminal.hpp>

namespace firmius::tui {

namespace {

std::vector<HelpItem> buildNavigationItems() {
  return {{"↑/↓", "Scroll chat"},
          {"PgUp/PgDn", "Page scroll"},
          {"Home/End", "Jump to start/end"},
          {"Esc", "Close modal / abort current run"}};
}

std::vector<HelpItem> buildAgentControlItems() {
  return {{"Ctrl+P", "Focus parent agent"},
          {GetHotkeyLabel(HotkeyAction::RetryLastRequest),
           "Retry/resume the stopped focused agent"},
          {GetHotkeyLabel(HotkeyAction::PermissionCycle),
           "Cycle thread permissions"},
          {GetHotkeyLabel(HotkeyAction::VariantCycle),
           "Cycle model variant on focused agent"},
          {"Ctrl+N", "Next sibling agent"},
          {"Ctrl+B", "Previous sibling agent"},
          {"Ctrl+F", "Focus owned process"}};
}

std::vector<HelpItem> buildCommandItems() {
  std::vector<HelpItem> items;
  for (const auto &entry : CommandManager::instance().listCommands()) {
    items.push_back({"/" + entry.name, entry.description});
    for (const auto &hint : entry.binding_hints) {
      if (!hint.label.empty() && !hint.description.empty()) {
        items.push_back({hint.label, hint.description});
      }
    }
  }
  return items;
}

std::vector<HelpItem> buildInputUiItems() {
  return {{"?", "Open help when the input is empty"},
          {GetHotkeyLabel(HotkeyAction::OpenHelp),
           "Open help from anywhere"},
          {GetHotkeyLabel(HotkeyAction::OpenCommandPalette),
           "Open command palette / launcher"},
          {"Ctrl+H", "Toggle notifications"},
          {"Ctrl+E", "Toggle edit mode / re-edit selected user message"},
          {"Ctrl+O", "Cycle work-lane tabs"},
          {"F6", "Show/hide agent strip"},
          {"F7", "Show/hide work panel"},
          {"Drag separators",
           "Resize work panel or agent strip height"},
          {"Ctrl+V", "Paste image from clipboard"},
          {GetHotkeyLabel(HotkeyAction::TranscriptUndo),
           "Undo last agent turn"},
          {GetHotkeyLabel(HotkeyAction::TranscriptUndoToUserBoundary),
           "Undo to last user message"},
          {GetHotkeyLabel(HotkeyAction::TranscriptRedo),
           "Redo last transcript undo"},
          {GetHotkeyLabel(HotkeyAction::EditUndo),
           "Undo last persisted file edit batch"},
          {GetHotkeyLabel(HotkeyAction::EditRedo),
           "Redo last persisted file edit undo"},
          {"Enter", "Send message"},
          {"Shift+Enter", "Insert newline"}};
}

std::vector<std::pair<std::string, std::vector<HelpItem>>> buildSections() {
  return {{"Navigation", buildNavigationItems()},
          {"Agent Control", buildAgentControlItems()},
          {"Commands", buildCommandItems()},
          {"Input + UI", buildInputUiItems()}};
}

} // namespace

std::vector<HelpItem> BuildHelpItemsForSection(const std::string &section_name) {
  for (const auto &[name, items] : buildSections()) {
    if (name == section_name) {
      return items;
    }
  }
  return {};
}

ftxui::Component HelpOverlay(TuiState &state) {
  auto scroll_content = ftxui::Renderer([] {
    const auto &theme = ThemeManager::instance().getCurrentTheme();
    auto section = [&](const std::string &title, const std::vector<HelpItem> &items) {
      ftxui::Elements rows;
      rows.push_back(ftxui::text(title) | ftxui::bold |
                     ftxui::color(theme.base.highlight));
      for (const auto &item : items) {
        rows.push_back(ftxui::hbox({
            ftxui::text(" " + item.key + " ") | ftxui::bold |
                ftxui::color(theme.modals.highlight_fg) |
                ftxui::bgcolor(theme.modals.highlight_bg),
            ftxui::text("  " + item.description) |
                ftxui::color(theme.modals.fg) | ftxui::flex,
        }));
      }
      return ftxui::vbox(rows);
    };

    ftxui::Elements body = {
        ftxui::text("FIRMIUS CONTROL SURFACE") | ftxui::bold |
            ftxui::color(theme.modals.title),
        ftxui::text(
            "A fullscreen quick-reference for navigation, thread control, input, "
            "and command launch.") |
            ftxui::color(theme.base.dim),
    };

    const auto sections = buildSections();
    for (size_t i = 0; i < sections.size(); ++i) {
      body.push_back(ftxui::separator());
      body.push_back(section(sections[i].first, sections[i].second));
    }

    body.push_back(ftxui::separator());
    body.push_back(ftxui::hbox({
                       ftxui::text(" [Esc] ") | ftxui::bold |
                           ftxui::color(theme.modals.highlight_fg) |
                           ftxui::bgcolor(theme.modals.highlight_bg),
                       ftxui::text("Close this overlay") |
                           ftxui::color(theme.modals.fg),
                   }) |
                   ftxui::center);

    return ftxui::vbox(std::move(body));
  });

  auto scrollable = ScrollableBox(scroll_content);
  auto component = ftxui::Renderer(scrollable, [scrollable] {
    const auto &theme = ThemeManager::instance().getCurrentTheme();
    const auto layout = ComputeHelpOverlayLayout(ftxui::Terminal::Size().dimx,
                                                 ftxui::Terminal::Size().dimy);
    auto body = ModalSection(
        theme,
        (scrollable->Render() | ftxui::xflex | ftxui::yflex | ftxui::yframe |
         ftxui::vscroll_indicator),
        theme.modals.bg);
    return FlatModalPanel(theme, "Help",
                          body | ftxui::size(ftxui::WIDTH, ftxui::EQUAL,
                                             layout.width - 4) |
                              ftxui::size(ftxui::HEIGHT, ftxui::EQUAL,
                                          layout.height - 4),
                          layout.width, layout.height, theme.modals.title);
  });
  return ftxui::CatchEvent(component, [&state, scrollable](ftxui::Event event) {
    if (event == ftxui::Event::Escape) {
      state.popModal();
      return true;
    }
    return scrollable->OnEvent(event);
  });
}

} // namespace firmius::tui

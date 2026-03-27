#include "components/HelpOverlay.hpp"
#include "TUIState.hpp"
#include "TUIHotkeys.hpp"
#include "ThemeManager.hpp"
#include "components/ScrollableBox.hpp"
#include "modals/ModalLayout.hpp"
#include <ftxui/component/component.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/terminal.hpp>

namespace firmius::tui {

ftxui::Component HelpOverlay(TuiState &state) {
  auto scroll_content = ftxui::Renderer([] {
    const auto &theme = ThemeManager::instance().getCurrentTheme();
    auto section = [&](const std::string &title,
                       const std::vector<std::pair<std::string, std::string>>
                           &items) {
      ftxui::Elements rows;
      rows.push_back(ftxui::text(title) | ftxui::bold |
                     ftxui::color(theme.base.highlight));
      for (const auto &[key, desc] : items) {
        rows.push_back(ftxui::hbox({
            ftxui::text(" " + key + " ") | ftxui::bold |
                ftxui::color(theme.modals.highlight_fg) |
                ftxui::bgcolor(theme.modals.highlight_bg),
            ftxui::text("  " + desc) | ftxui::color(theme.modals.fg) |
                ftxui::flex,
        }));
      }
      return ftxui::vbox(rows);
    };

    return ftxui::vbox({
        ftxui::text("FIRMIUS CONTROL SURFACE") | ftxui::bold |
            ftxui::color(theme.modals.title),
        ftxui::text(
            "A fullscreen quick-reference for navigation, thread control, and "
            "input behavior.") |
            ftxui::color(theme.base.dim),
        ftxui::separator(),
        section("Navigation",
                {{"↑/↓", "Scroll chat"},
                 {"PgUp/PgDn", "Page scroll"},
                 {"Home/End", "Jump to start/end"},
                 {"Esc", "Close modal / abort current run"}}),
        ftxui::separator(),
        section("Agent Control",
                {{"Ctrl+P", "Focus parent agent"},
                 {kRetryLastRequestHotkeyLabel,
                  "Retry/resume the stopped focused agent"},
                 {kPermissionCycleHotkeyLabel, "Cycle thread permissions"},
                 {kVariantCycleHotkeyLabel,
                  "Cycle model variant on focused agent"},
                 {"/config then P", "Fallback permission mode cycle"},
                 {"Ctrl+N", "Next sibling agent"},
                 {"Ctrl+B", "Previous sibling agent"},
                 {"Ctrl+F", "Focus owned process"}}),
        ftxui::separator(),
        section("Commands",
                {{"/threads", "List threads"},
                 {"/new", "New thread"},
                 {"/benchmarks", "Run benchmark (starts benchmark thread)"},
                 {"/models", "Pick model"},
                 {"/router", "Manage model routing categories"},
                 {"/purposes", "Map personas to model categories"},
                 {"/config", "View config"},
                 {"/connect", "Connect provider"},
                 {"/accounts", "Manage accounts"},
                 {"/quotas", "View quotas"}}),
        ftxui::separator(),
        section("Input + UI",
                {{"?", "Open help when the input is empty"},
                 {"F1", "Open help from anywhere"},
                 {"Ctrl+H", "Toggle notifications"},
                 {"Ctrl+O", "Toggle PLAN/TODO panel (or expand plan)"},
                 {"Ctrl+V", "Paste image from clipboard"},
                 {"Enter", "Send message"},
                 {"Shift+Enter", "Insert newline"}}),
        ftxui::separator(),
        ftxui::hbox({
            ftxui::text(" [Esc] ") | ftxui::bold |
                ftxui::color(theme.modals.highlight_fg) |
                ftxui::bgcolor(theme.modals.highlight_bg),
            ftxui::text("Close this overlay") | ftxui::color(theme.modals.fg),
        }) | ftxui::center,
    });
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

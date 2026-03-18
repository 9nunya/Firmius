#include "components/HelpOverlay.hpp"
#include "components/GlintEffect.hpp"
#include "TUIHotkeys.hpp"
#include <ftxui/component/component.hpp>
#include <ftxui/dom/elements.hpp>

namespace firmius::tui {

ftxui::Component HelpOverlay() {
  auto render_help = []() -> ftxui::Element {
    auto section = [](const std::string& title, const std::vector<std::pair<std::string, std::string>>& items) {
      ftxui::Elements rows;
      rows.push_back(ftxui::text(title) | ftxui::bold | ftxui::color(ftxui::Color::Cyan) | ftxui::underlined);
      for (const auto& [key, desc] : items) {
        rows.push_back(ftxui::hbox({
          ftxui::text(" " + key + " ") | ftxui::bold | ftxui::color(ftxui::Color::Yellow),
          ftxui::filler(),
          ftxui::text(desc + " ") | ftxui::dim
        }));
      }
      return ftxui::vbox(rows);
    };
    
    auto help_content = ftxui::vbox({
      ftxui::text(" Keyboard Shortcuts ") | ftxui::bold | ftxui::color(ftxui::Color::White) | ftxui::bgcolor(ftxui::Color::RGB(60, 60, 100)),
      ftxui::separator(),
      
      section("Navigation", {
        {"↑/↓", "Scroll chat"},
        {"PgUp/PgDn", "Page scroll"},
        {"Home/End", "Jump to start/end"},
        {"Esc", "Close modal / Abort"},
      }),
      
      ftxui::separator(),
      
      section("Agent Control", {
        {"Ctrl+P", "Focus parent agent"},
        {kPermissionCycleHotkeyLabel, "Cycle thread permissions"},
        {"/config then P", "Fallback permission mode cycle"},
        {"Ctrl+N", "Next sibling agent"},
        {"Ctrl+B", "Previous sibling agent"},
        {"Ctrl+F", "Focus on process (interactive)"},
      }),
      
      ftxui::separator(),
      
      section("Commands", {
        {"/threads", "List threads"},
        {"/new", "New thread"},
        {"/models", "Pick model"},
        {"/config", "View config"},
        {"/connect", "Connect provider"},
        {"/accounts", "Manage accounts"},
        {"/quotas", "View quotas"},
      }),
      
      ftxui::separator(),
      
      section("UI", {
        {"?", "Toggle this help"},
        {"Ctrl+H", "Toggle notifications"},
        {"Ctrl+O", "Expand/collapse plan lane"},
        {"Enter", "Send message"},
        {"Shift+Enter", "New line"},
      }),
      
      ftxui::separator(),
      ftxui::text(" Press ? to close ") | ftxui::dim | ftxui::center,
    });
    
    return ftxui::dbox({
      ftxui::text("") | ftxui::clear_under,
      help_content | ftxui::center | ftxui::clear_under
    }) | ftxui::clear_under;
  };

  return ftxui::Renderer([render_help] {
    return render_help();
  });
}

} // namespace firmius::tui

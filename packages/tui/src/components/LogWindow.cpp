#include "components/LogWindow.hpp"
#include "ThemeManager.hpp"

namespace firmius::tui {

ftxui::Element LogWindow(const std::vector<ftxui::Element> &lines,
                         const std::string &footer_label,
                         ftxui::Element action_element) {
  const auto &theme = ThemeManager::instance().getCurrentTheme();
  auto body = ftxui::vbox(lines) |
              ftxui::bgcolor(theme.base.bg) |
              ftxui::color(theme.base.fg) | ftxui::xflex;

  ftxui::Elements rows;
  if (!footer_label.empty()) {
    std::vector<ftxui::Element> footer_parts;
    footer_parts.push_back(ftxui::text(" ") |
                           ftxui::bgcolor(theme.tool_blocks.specific.terminal.fg));
    footer_parts.push_back(ftxui::text(" ") |
                           ftxui::color(theme.tool_blocks.specific.terminal.fg));
    footer_parts.push_back(ftxui::text(footer_label) | ftxui::bold |
                           ftxui::color(theme.base.fg));
    footer_parts.push_back(ftxui::filler());
    footer_parts.push_back(action_element);
    rows.push_back(ftxui::hbox(std::move(footer_parts)) |
                   ftxui::bgcolor(theme.tool_blocks.generic_header_bg) |
                   ftxui::xflex);
  }

  rows.push_back(ftxui::text(" ") | ftxui::bgcolor(theme.base.bg));
  rows.push_back(body);
  rows.push_back(ftxui::text(" ") | ftxui::bgcolor(theme.base.bg));
  return ftxui::vbox(std::move(rows)) |
         ftxui::bgcolor(theme.tool_blocks.generic_bg) | ftxui::xflex;
}

}

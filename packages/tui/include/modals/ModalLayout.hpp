#pragma once

#include "Theme.hpp"
#include <ftxui/dom/elements.hpp>
#include <string>

namespace firmius::tui {

inline ftxui::Element ModalPanelPadding(ftxui::Element child) {
  return ftxui::vbox({
      ftxui::text(""),
      ftxui::hbox({
          ftxui::text("  "),
          std::move(child),
          ftxui::text("  "),
      }),
      ftxui::text(""),
  });
}

inline ftxui::Element ModalSection(const Theme &theme, ftxui::Element child,
                                   ftxui::Color background) {
  return ModalPanelPadding(std::move(child)) | ftxui::bgcolor(background) |
         ftxui::color(theme.modals.fg);
}

inline ftxui::Element FlatModalPanel(const Theme &theme, const std::string &title,
                                     ftxui::Element body, int max_width = 60,
                                     int max_height = 22,
                                     ftxui::Color title_color = ftxui::Color::Default) {
  if (title_color == ftxui::Color::Default) {
    title_color = theme.modals.title;
  }

  auto panel = ftxui::vbox({
                   ModalSection(
                       theme,
                       ftxui::text(title) | ftxui::bold |
                           ftxui::color(title_color),
                       theme.modals.highlight_bg),
                   body,
               }) |
               ftxui::bgcolor(theme.modals.bg) | ftxui::color(theme.modals.fg) |
               ftxui::clear_under;

  if (max_width > 0) {
    panel = panel | ftxui::size(ftxui::WIDTH, ftxui::LESS_THAN, max_width);
  }
  if (max_height > 0) {
    panel = panel | ftxui::size(ftxui::HEIGHT, ftxui::LESS_THAN, max_height);
  }
  return panel | ftxui::center;
}

} // namespace firmius::tui

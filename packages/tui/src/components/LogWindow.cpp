#include "components/LogWindow.hpp"

namespace firmius::tui {

ftxui::Element LogWindow(const std::vector<ftxui::Element> &lines,
                          const std::string &footer_label,
                          const std::string &action_label) {
  auto body = ftxui::vbox(lines);

  // Compact footer - only show label if present
  if (!footer_label.empty()) {
    std::vector<ftxui::Element> footer_parts;
    footer_parts.push_back(ftxui::text(" " + footer_label) | ftxui::dim);
    if (!action_label.empty()) {
      footer_parts.push_back(ftxui::text(" [") | ftxui::dim);
      footer_parts.push_back(ftxui::text(action_label) | ftxui::dim);
      footer_parts.push_back(ftxui::text("]") | ftxui::dim);
    }
    auto footer = ftxui::hbox(std::move(footer_parts));
    return ftxui::vbox({body | ftxui::borderLight, footer});
  }

  return body | ftxui::borderLight;
}

}

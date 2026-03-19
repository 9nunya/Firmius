#ifndef FIRMIUS_TUI_COMPONENTS_LOG_WINDOW_HPP
#define FIRMIUS_TUI_COMPONENTS_LOG_WINDOW_HPP

#include <ftxui/dom/elements.hpp>
#include <string>
#include <vector>

namespace firmius::tui {

ftxui::Element LogWindow(const std::vector<ftxui::Element> &lines,
                         const std::string &footer_label,
                         ftxui::Element action_element = ftxui::text(""));

}

#endif

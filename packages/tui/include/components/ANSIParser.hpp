#ifndef FIRMIUS_TUI_ANSI_PARSER_HPP
#define FIRMIUS_TUI_ANSI_PARSER_HPP

#include <ftxui/dom/elements.hpp>
#include <string>
#include <vector>

namespace firmius::tui {

// Parse ANSI escape codes and return styled elements
ftxui::Element ParseANSI(const std::string& text);

// Split text into lines, preserving ANSI codes per line
std::vector<ftxui::Element> ParseANSILines(const std::string& text);

} // namespace firmius::tui

#endif

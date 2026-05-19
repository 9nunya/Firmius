#pragma once

#include "Cell.hpp"

#include <string>
#include <vector>

namespace firmius::tui {

/// Parses ANSI-styled text into a row of Cells.
///
/// Only handles SGR (Select Graphic Rendition) sequences for color/style.
/// Cursor movement sequences are ignored. Processes one line at a time
/// (newlines are ignored).
class AnsiParser {
public:
  /// Parse a styled string into a row of Cells.
  /// @param text ANSI-styled text (no newlines)
  /// @param maxWidth Maximum cells to produce (-1 = unlimited)
  static std::vector<Cell> parse(const std::string& text, int maxWidth = -1);
};

} // namespace firmius::tui

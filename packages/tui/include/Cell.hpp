#pragma once

#include <compare>
#include <cstdint>
#include <vector>

namespace firmius::tui {

/// Color for a cell foreground or background.
struct CellColor {
  enum class Type : uint8_t { Default, Palette256, RGB };
  Type type = Type::Default;
  uint8_t index = 0;             // Palette256 index
  uint8_t r = 0, g = 0, b = 0;  // RGB components

  auto operator<=>(const CellColor&) const = default;
};

/// Style attributes for a cell.
struct CellStyle {
  bool bold = false;
  bool dim = false;
  bool italic = false;
  bool underline = false;
  bool strikethrough = false;
  bool invert = false;

  auto operator<=>(const CellStyle&) const = default;
};

/// A single character cell on the terminal screen.
struct Cell {
  char32_t ch = ' ';  // Unicode codepoint (space = empty/blank)
  CellColor fg;
  CellColor bg;
  CellStyle style;

  auto operator<=>(const Cell&) const = default;

  bool isEmpty() const {
    return ch == ' ' && fg.type == CellColor::Type::Default &&
           bg.type == CellColor::Type::Default &&
           !style.bold && !style.dim && !style.italic &&
           !style.underline && !style.strikethrough && !style.invert;
  }
};

/// A 2D grid of cells: rows × columns.
using CellGrid = std::vector<std::vector<Cell>>;

} // namespace firmius::tui

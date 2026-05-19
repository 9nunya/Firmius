#include "AnsiParser.hpp"

#include <cctype>

namespace firmius::tui {

namespace {

// Decode a UTF-8 sequence starting at text[pos]. Advances pos.
// Returns the codepoint, or U+FFFD on malformed sequences.
char32_t decodeUtf8(const std::string& text, size_t& pos) {
  if (pos >= text.size()) return U'\xFFFD';

  auto b = static_cast<uint8_t>(text[pos]);
  char32_t cp;
  int extra;

  if (b < 0x80) {
    cp = b; extra = 0;
  } else if ((b & 0xE0) == 0xC0) {
    cp = b & 0x1F; extra = 1;
  } else if ((b & 0xF0) == 0xE0) {
    cp = b & 0x0F; extra = 2;
  } else if ((b & 0xF8) == 0xF0) {
    cp = b & 0x07; extra = 3;
  } else {
    ++pos;
    return U'\xFFFD'; // invalid leading byte
  }

  for (int i = 0; i < extra; ++i) {
    ++pos;
    if (pos >= text.size()) return U'\xFFFD';
    b = static_cast<uint8_t>(text[pos]);
    if ((b & 0xC0) != 0x80) return U'\xFFFD';
    cp = (cp << 6) | (b & 0x3F);
  }

  ++pos;
  return cp;
}

// Parse a single SGR parameter value from the param list.
// Returns true if a color sequence (38/48) was consumed (advancing i past its args).
bool applySgrParam(const std::vector<int>& params, size_t& i,
                   CellColor& fg, CellColor& bg, CellStyle& style) {
  if (i >= params.size()) return false;
  int p = params[i];

  switch (p) {
    case 0: // reset
      fg = {}; bg = {}; style = {};
      break;
    case 1: style.bold = true; break;
    case 2: style.dim = true; break;
    case 3: style.italic = true; break;
    case 4: style.underline = true; break;
    case 7: style.invert = true; break;
    case 9: style.strikethrough = true; break;
    case 22: style.bold = false; style.dim = false; break;
    case 23: style.italic = false; break;
    case 24: style.underline = false; break;
    case 27: style.invert = false; break;
    case 29: style.strikethrough = false; break;
    case 39: fg = {}; break; // default fg
    case 49: bg = {}; break; // default bg
    default:
      // Standard fg colors 30-37
      if (p >= 30 && p <= 37) {
        fg.type = CellColor::Type::Palette256;
        fg.index = static_cast<uint8_t>(p - 30);
      }
      // 256-color or RGB fg
      else if (p == 38) {
        if (i + 1 < params.size() && params[i + 1] == 5 && i + 2 < params.size()) {
          fg.type = CellColor::Type::Palette256;
          fg.index = static_cast<uint8_t>(params[i + 2]);
          i += 2;
          return true;
        } else if (i + 1 < params.size() && params[i + 1] == 2 && i + 4 < params.size()) {
          fg.type = CellColor::Type::RGB;
          fg.r = static_cast<uint8_t>(params[i + 2]);
          fg.g = static_cast<uint8_t>(params[i + 3]);
          fg.b = static_cast<uint8_t>(params[i + 4]);
          i += 4;
          return true;
        }
      }
      // Standard bg colors 40-47
      else if (p >= 40 && p <= 47) {
        bg.type = CellColor::Type::Palette256;
        bg.index = static_cast<uint8_t>(p - 40);
      }
      // 256-color or RGB bg
      else if (p == 48) {
        if (i + 1 < params.size() && params[i + 1] == 5 && i + 2 < params.size()) {
          bg.type = CellColor::Type::Palette256;
          bg.index = static_cast<uint8_t>(params[i + 2]);
          i += 2;
          return true;
        } else if (i + 1 < params.size() && params[i + 1] == 2 && i + 4 < params.size()) {
          bg.type = CellColor::Type::RGB;
          bg.r = static_cast<uint8_t>(params[i + 2]);
          bg.g = static_cast<uint8_t>(params[i + 3]);
          bg.b = static_cast<uint8_t>(params[i + 4]);
          i += 4;
          return true;
        }
      }
      break;
  }
  return false;
}

} // namespace

std::vector<Cell> AnsiParser::parse(const std::string& text, int maxWidth) {
  std::vector<Cell> cells;
  cells.reserve(text.size()); // upper bound

  CellColor fg, bg;
  CellStyle style;

  enum class State { Normal, Esc, CsiParam, CsiIntermediate };
  State state = State::Normal;
  std::string paramBuf;

  size_t pos = 0;
  while (pos < text.size()) {
    if (maxWidth >= 0 && static_cast<int>(cells.size()) >= maxWidth) break;

    uint8_t ch = static_cast<uint8_t>(text[pos]);

    switch (state) {
      case State::Normal:
        if (ch == 0x1B) { // ESC
          state = State::Esc;
          ++pos;
        } else if (ch == '\n' || ch == '\r' || ch == '\t') {
          // Skip whitespace control chars (we process one line at a time)
          ++pos;
        } else if (ch >= 0x20) {
          // Printable character — decode UTF-8 and emit cell
          char32_t cp = decodeUtf8(text, pos);
          cells.push_back({cp, fg, bg, style});
        } else {
          ++pos; // Skip other control chars
        }
        break;

      case State::Esc:
        if (ch == '[') { // CSI
          state = State::CsiParam;
          paramBuf.clear();
          ++pos;
        } else {
          // Not a CSI sequence — skip and return to normal
          state = State::Normal;
          ++pos;
        }
        break;

      case State::CsiParam:
        if (ch >= '0' && ch <= '?') {
          // Parameter byte (accumulate)
          paramBuf.push_back(static_cast<char>(ch));
          ++pos;
        } else if (ch >= 0x40 && ch <= 0x7E) {
          // Final byte
          if (ch == 'm') { // SGR
            // Parse semicolon-separated parameters
            std::vector<int> params;
            int val = 0;
            bool hasVal = false;
            for (char c : paramBuf) {
              if (c == ';') {
                params.push_back(hasVal ? val : 0);
                val = 0;
                hasVal = false;
              } else if (c >= '0' && c <= '9') {
                val = val * 10 + (c - '0');
                hasVal = true;
              }
              // Skip other chars (e.g., '?', '<', '>' — not SGR)
            }
            if (hasVal || !paramBuf.empty()) {
              params.push_back(hasVal ? val : 0);
            }

            // Apply parameters
            for (size_t i = 0; i < params.size(); ++i) {
              applySgrParam(params, i, fg, bg, style);
            }
          }
          // Any other final byte (cursor movement, etc.) — ignore
          state = State::Normal;
          ++pos;
        } else if (ch >= 0x20 && ch <= 0x3F) {
          // Intermediate byte — skip
          ++pos;
        } else {
          // Invalid — abort CSI
          state = State::Normal;
          ++pos;
        }
        break;

      default:
        state = State::Normal;
        ++pos;
        break;
    }
  }

  return cells;
}

} // namespace firmius::tui

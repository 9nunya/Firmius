#include "items/StreamingItems.hpp"
#include "Terminal.hpp"
#include "ThemeAnsi.hpp"
#include "ThemeManager.hpp"

#include <algorithm>
#include <cctype>
#include <sstream>

namespace firmius::tui2 {

namespace {

// ── Inline markdown ──

std::string renderInlineMarkdown(const std::string& text) {
  const auto& theme = ThemeManager::instance().currentTheme();
  std::string out;
  out.reserve(text.size());

  for (size_t i = 0; i < text.size();) {
    // Inline code
    if (text[i] == '`') {
      size_t end = text.find('`', i + 1);
      if (end != std::string::npos) {
        out += ansi::bgRgb(theme.base.separator.r, theme.base.separator.g,
                           theme.base.separator.b,
                           ansi::fgRgb(theme.base.fg.r, theme.base.fg.g,
                                       theme.base.fg.b,
                                       text.substr(i + 1, end - i - 1)));
        i = end + 1;
        continue;
      }
    }
    // Links: [text](url)
    if (text[i] == '[') {
      size_t bracket = text.find(']', i + 1);
      if (bracket != std::string::npos && bracket + 1 < text.size() && text[bracket + 1] == '(') {
        size_t paren = text.find(')', bracket + 2);
        if (paren != std::string::npos) {
          std::string linkText = text.substr(i + 1, bracket - i - 1);
          std::string url = text.substr(bracket + 2, paren - bracket - 2);
          out += ansi::underline(linkText) + theme_ansi::dim(" (" + url + ")");
          i = paren + 1;
          continue;
        }
      }
    }
    // Bold
    if (i + 1 < text.size() && text[i] == '*' && text[i + 1] == '*') {
      size_t end = text.find("**", i + 2);
      if (end != std::string::npos) {
        out += ansi::bold(renderInlineMarkdown(text.substr(i + 2, end - i - 2)));
        i = end + 2;
        continue;
      }
    }
    // Strikethrough
    if (i + 1 < text.size() && text[i] == '~' && text[i + 1] == '~') {
      size_t end = text.find("~~", i + 2);
      if (end != std::string::npos) {
        out += ansi::strikethrough(text.substr(i + 2, end - i - 2));
        i = end + 2;
        continue;
      }
    }
    // Italic
    if (text[i] == '*') {
      size_t end = text.find('*', i + 1);
      if (end != std::string::npos) {
        out += ansi::italic(renderInlineMarkdown(text.substr(i + 1, end - i - 1)));
        i = end + 1;
        continue;
      }
    }
    out.push_back(text[i++]);
  }
  return out;
}

// ── Block-level markdown helpers ──

bool isHorizontalRule(const std::string& line) {
  // ---, ***, ___ with optional spaces, 3+ of the same char, nothing else
  int count = 0;
  char marker = 0;
  for (char c : line) {
    if (c == ' ') continue;
    if (marker == 0) {
      if (c == '-' || c == '*' || c == '_') {
        marker = c;
        count = 1;
      } else {
        return false;
      }
    } else if (c == marker) {
      ++count;
    } else {
      return false;
    }
  }
  return count >= 3;
}

bool isSetextUnderline(const std::string& line, char& marker) {
  if (line.empty()) return false;
  marker = line[0];
  if (marker != '=' && marker != '-') return false;
  for (char c : line) {
    if (c != marker && c != ' ') return false;
  }
  return true;
}

// ── Table support ──

enum class Alignment { Left, Center, Right };

struct TableRow {
  std::vector<std::string> cells;
};

/// Split a table row by | into trimmed cells.
TableRow parseTableRow(const std::string& line) {
  TableRow row;
  std::string cell;
  // Skip leading |
  size_t start = (line.size() > 0 && line[0] == '|') ? 1 : 0;
  for (size_t i = start; i <= line.size(); ++i) {
    if (i == line.size() || line[i] == '|') {
      // Trim
      size_t b = 0, e = cell.size();
      while (b < e && cell[b] == ' ') ++b;
      while (e > b && cell[e - 1] == ' ') --e;
      row.cells.push_back(cell.substr(b, e - b));
      cell.clear();
    } else {
      cell += line[i];
    }
  }
  // Strip trailing empty cell from trailing |
  if (!row.cells.empty() && row.cells.back().empty()) row.cells.pop_back();
  return row;
}

bool isTableSeparator(const std::string& line) {
  // | --- | :---: | ---: | — must be only dashes, colons, pipes, spaces
  for (char c : line) {
    if (c != '-' && c != ':' && c != '|' && c != ' ') return false;
  }
  // Must contain at least one -
  return line.find('-') != std::string::npos;
}

std::vector<Alignment> parseTableAlignments(const std::string& sepLine) {
  TableRow sep = parseTableRow(sepLine);
  std::vector<Alignment> aligns;
  for (const auto& cell : sep.cells) {
    bool leftColon = !cell.empty() && cell.front() == ':';
    bool rightColon = !cell.empty() && cell.back() == ':';
    if (leftColon && rightColon)
      aligns.push_back(Alignment::Center);
    else if (rightColon)
      aligns.push_back(Alignment::Right);
    else
      aligns.push_back(Alignment::Left);
  }
  return aligns;
}

bool isTableRow(const std::string& line) {
  if (line.empty()) return false;
  // Must contain at least one |
  return line.find('|') != std::string::npos;
}

/// Render an aligned cell content padded to the given width.
std::string alignCell(const std::string& content, int width, Alignment align) {
  int visW = ansi::visibleWidth(content);
  if (visW >= width) return content;
  int pad = width - visW;
  switch (align) {
    case Alignment::Left:
      return content + std::string(pad, ' ');
    case Alignment::Right:
      return std::string(pad, ' ') + content;
    case Alignment::Center: {
      int left = pad / 2;
      int right = pad - left;
      return std::string(left, ' ') + content + std::string(right, ' ');
    }
  }
  return content; // unreachable
}

/// Render accumulated table lines into ANSI-formatted output lines.
std::vector<std::string> renderTable(const std::vector<std::string>& rawLines) {
  if (rawLines.size() < 2) return {}; // need at least header + separator

  // Parse header
  TableRow header = parseTableRow(rawLines[0]);
  int numCols = static_cast<int>(header.cells.size());
  if (numCols == 0) return {};

  // Parse separator alignments
  std::vector<Alignment> aligns = parseTableAlignments(rawLines[1]);
  while (static_cast<int>(aligns.size()) < numCols) aligns.push_back(Alignment::Left);

  // Parse body rows (skip separator at index 1)
  std::vector<TableRow> bodyRows;
  for (size_t i = 2; i < rawLines.size(); ++i) {
    bodyRows.push_back(parseTableRow(rawLines[i]));
  }

  // Calculate column widths
  std::vector<int> colWidths(numCols, 0);
  for (int c = 0; c < numCols; ++c) {
    colWidths[c] = static_cast<int>(header.cells[c].size());
    for (auto& row : bodyRows) {
      if (c < static_cast<int>(row.cells.size())) {
        colWidths[c] = std::max(colWidths[c], static_cast<int>(row.cells[c].size()));
      }
    }
    colWidths[c] = std::max(colWidths[c], 3); // minimum width
  }

  // ANSI border style
  auto border = [&](const std::string& s) {
    return theme_ansi::dim(s);
  };

  // Helper: repeat a UTF-8 string n times
  auto repeatUtf8 = [](const std::string& s, int n) -> std::string {
    std::string r;
    r.reserve(s.size() * static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) r += s;
    return r;
  };

  const std::string hLine = "\xe2\x94\x80"; // ─
  // Cell content is padded with " " on each side, so border segment = colWidth + 2
  auto segWidth = [&](int c) { return colWidths[c] + 2; };

  // Build separator line: ├───┼───┤
  std::string sepLine = border("\xe2\x94\x9c"); // ├
  for (int c = 0; c < numCols; ++c) {
    sepLine += border(repeatUtf8(hLine, segWidth(c)));
    if (c < numCols - 1)
      sepLine += border("\xe2\x94\xbc"); // ┼
    else
      sepLine += border("\xe2\x94\xa4"); // ┤
  }

  // Build top border: ┌───┬───┐
  std::string topLine = border("\xe2\x94\x8c"); // ┌
  for (int c = 0; c < numCols; ++c) {
    topLine += border(repeatUtf8(hLine, segWidth(c)));
    if (c < numCols - 1)
      topLine += border("\xe2\x94\xac"); // ┬
    else
      topLine += border("\xe2\x94\x90"); // ┐
  }

  // Build bottom border: └───┴───┘
  std::string botLine = border("\xe2\x94\x94"); // └
  for (int c = 0; c < numCols; ++c) {
    botLine += border(repeatUtf8(hLine, segWidth(c)));
    if (c < numCols - 1)
      botLine += border("\xe2\x94\xb4"); // ┴
    else
      botLine += border("\xe2\x94\x98"); // ┘
  }

  // Render function for a data row
  auto renderRow = [&](const TableRow& row, bool isHeader) -> std::string {
    std::string out = border("\xe2\x94\x82"); // │
    for (int c = 0; c < numCols; ++c) {
      std::string cellText = (c < static_cast<int>(row.cells.size())) ? row.cells[c] : "";
      cellText = renderInlineMarkdown(cellText);
      if (isHeader) cellText = ansi::bold(cellText);
      out += " " + alignCell(cellText, colWidths[c], aligns[c]) + " ";
      out += border("\xe2\x94\x82"); // │
    }
    return out;
  };

  std::vector<std::string> result;
  result.push_back(topLine);
  result.push_back(renderRow(header, true));
  result.push_back(sepLine);
  for (auto& row : bodyRows) {
    result.push_back(renderRow(row, false));
  }
  result.push_back(botLine);
  return result;
}

/// Check if a line is a task list item: `- [ ]` or `- [x]` or `- [X]`
bool isTaskListItem(const std::string& line, bool& checked, std::string& rest) {
  size_t i = 0;
  while (i < line.size() && line[i] == ' ') ++i;
  if (i + 5 > line.size()) return false;
  if (line[i] != '-' || line[i + 1] != ' ') return false;
  i += 2;
  if (line[i] != '[') return false;
  char ch = line[i + 1];
  if (ch != ' ' && ch != 'x' && ch != 'X') return false;
  if (line[i + 2] != ']') return false;
  checked = (ch == 'x' || ch == 'X');
  i += 3;
  if (i < line.size() && line[i] == ' ') ++i;
  rest = line.substr(i);
  return true;
}

/// Check if a line starts a numbered list: `N. text`
bool isNumberedList(const std::string& line, std::string& num, std::string& rest) {
  size_t i = 0;
  while (i < line.size() && line[i] == ' ') ++i;
  size_t numStart = i;
  while (i < line.size() && std::isdigit(static_cast<unsigned char>(line[i]))) ++i;
  if (i == numStart) return false; // no digits
  if (i >= line.size() || line[i] != '.') return false;
  num = line.substr(numStart, i - numStart);
  ++i; // skip dot
  if (i >= line.size() || line[i] != ' ') return false;
  ++i; // skip space
  rest = line.substr(i);
  return true;
}

// Forward declaration
int visualWidth(const std::string& s);

// ── Main block renderer ──

std::string renderMarkdownBlock(const std::string& text, bool dimmed, int width = 0) {
  // Collect all lines for index-based processing (setext headings need lookahead,
  // tables need batch accumulation)
  std::vector<std::string> lines;
  {
    std::istringstream stream(text);
    std::string line;
    while (std::getline(stream, line)) {
      // Strip trailing \r (streaming deltas may use \r\n line endings)
      if (!line.empty() && line.back() == '\r') line.pop_back();
      lines.push_back(line);
    }
  }

  std::string out;
  bool inFence = false;

  auto append = [&](std::string rendered) {
    if (dimmed) rendered = theme_ansi::dim(rendered);
    if (!out.empty()) out += '\n';
    out += rendered;
  };

  for (size_t i = 0; i < lines.size(); ++i) {
    const std::string& line = lines[i];

    // ── Code fences ──
    if (line.rfind("```", 0) == 0) {
      inFence = !inFence;
      // Don't render fence markers — just toggle state
      continue;
    }
    if (inFence) {
      // Pad code lines to full width so background extends to edge
      std::string padded = line;
      if (width > 0) {
        int lineW = visualWidth(line);
        int pad = width - lineW;
        if (pad > 0) padded += std::string(pad, ' ');
      }
      const auto& theme = ThemeManager::instance().currentTheme();
      append(ansi::bgRgb(theme.base.separator.r, theme.base.separator.g,
                         theme.base.separator.b,
                         ansi::fgRgb(theme.base.fg.r, theme.base.fg.g,
                                     theme.base.fg.b, padded)));
      continue;
    }

    // ── Tables: accumulate header + separator + body rows ──
    if (isTableRow(line) && i + 1 < lines.size() && isTableSeparator(lines[i + 1])) {
      std::vector<std::string> tableLines;
      tableLines.push_back(line);
      ++i;
      if (i < lines.size()) tableLines.push_back(lines[i]);
      ++i;
      while (i < lines.size() && isTableRow(lines[i])) {
        tableLines.push_back(lines[i]);
        ++i;
      }
      --i; // back up — for loop increments
      for (auto& tl : renderTable(tableLines)) append(tl);
      continue;
    }

    // ── Setext headings: line followed by === or --- ──
    if (i + 1 < lines.size() && !line.empty()) {
      char setextMarker;
      if (isSetextUnderline(lines[i + 1], setextMarker)) {
        append(ansi::bold(theme_ansi::accent(line)));
        ++i; // skip underline
        continue;
      }
    }

    // ── Horizontal rules ──
    if (isHorizontalRule(line)) {
      std::string rule;
      rule.reserve(180);
      for (int w = 0; w < 60; ++w) rule += "\xe2\x94\x80"; // ─
      append(theme_ansi::dim(rule));
      continue;
    }

    // ── ATX headings ──
    if (!line.empty() && line[0] == '#') {
      size_t hashes = 0;
      while (hashes < line.size() && line[hashes] == '#') ++hashes;
      if (hashes < line.size() && line[hashes] == ' ') {
        append(ansi::bold(theme_ansi::accent(line.substr(hashes + 1))));
        continue;
      }
      // Not a valid heading, fall through to inline
    }

    // ── Blockquotes ──
    if (line.rfind("> ", 0) == 0) {
      append(theme_ansi::dim("\xe2\x96\x8c " + line.substr(2)));
      continue;
    }

    // ── Task lists ──
    {
      bool checked = false;
      std::string taskRest;
      if (isTaskListItem(line, checked, taskRest)) {
        std::string checkbox = checked ? "\xe2\x98\x91 " : "\xe2\x98\x90 ";
        append(theme_ansi::accent("- " + checkbox) + renderInlineMarkdown(taskRest));
        continue;
      }
    }

    // ── Numbered lists ──
    {
      std::string num, listRest;
      if (isNumberedList(line, num, listRest)) {
        append(theme_ansi::accent(num + ". ") + renderInlineMarkdown(listRest));
        continue;
      }
    }

    // ── Unordered lists ──
    if (line.rfind("- ", 0) == 0 || line.rfind("* ", 0) == 0) {
      append(theme_ansi::accent("\xe2\x80\xa2 ") + renderInlineMarkdown(line.substr(2)));
      continue;
    }

    // ── Plain text ──
    append(renderInlineMarkdown(line));
  }
  return out;
}

// Measure visual width of a string, ignoring ANSI escape sequences.
// Counts UTF-8 code points (not bytes) — each code point = 1 column.
// Does not handle CJK double-width; good enough for box-drawing and latin text.
int visualWidth(const std::string& s) {
  int w = 0;
  for (size_t i = 0; i < s.size(); ) {
    if (s[i] == '\033' && i + 1 < s.size() && s[i + 1] == '[') {
      i += 2;
      while (i < s.size() && s[i] != 'm') ++i;
      if (i < s.size()) ++i;
    } else if ((static_cast<unsigned char>(s[i]) & 0xC0) == 0x80) {
      // UTF-8 continuation byte — skip, don't count
      ++i;
    } else {
      ++w;
      ++i;
    }
  }
  return w;
}

// Wrap a single rendered line to fit within maxWidth visual columns.
// Splits at word boundaries when possible, hard-splits if no space found.
// Preserves ANSI codes across splits.
// stylePrefix is reapplied to each continuation line (e.g. dim ANSI codes)
// so that styling persists across wrapped lines.
std::vector<std::string> wrapLine(const std::string& line, int maxWidth,
                                   const std::string& stylePrefix = "") {
  if (maxWidth <= 0) return {line};

  std::vector<std::string> result;
  std::string current;
  int currentWidth = 0;
  size_t lastSpacePos = 0;
  int lastSpaceWidth = 0;

  for (size_t i = 0; i < line.size(); ) {
    if (line[i] == '\033' && i + 1 < line.size() && line[i + 1] == '[') {
      size_t start = i;
      i += 2;
      while (i < line.size() && line[i] != 'm') ++i;
      if (i < line.size()) ++i;
      current += line.substr(start, i - start);
      continue;
    }

    if (line[i] == ' ') {
      lastSpacePos = current.size();
      lastSpaceWidth = currentWidth;
    }

    current += line[i];
    ++currentWidth;
    ++i;

    if (currentWidth >= maxWidth) {
      if (lastSpacePos > 0 && lastSpaceWidth > 0) {
        result.push_back(current.substr(0, lastSpacePos));
        current = current.substr(lastSpacePos + 1);
        currentWidth = visualWidth(current);
      } else {
        result.push_back(current);
        current.clear();
        currentWidth = 0;
      }
      // Re-apply style prefix so dim/etc. persists across wrapped lines.
      // Without this, the terminal resets styling at line boundaries and
      // continuation lines lose the dim effect.
      if (!stylePrefix.empty()) {
        current = stylePrefix + current;
        currentWidth = visualWidth(current);
      }
      lastSpacePos = 0;
      lastSpaceWidth = 0;
    }
  }
  if (!current.empty()) result.push_back(current);
  if (result.empty()) result.push_back("");
  return result;
}

std::vector<std::string> renderStreamingBlock(const std::string& text, int width,
                                               const std::string& prefix, bool dimmed) {
  std::vector<std::string> lines;
  std::string rendered = renderMarkdownBlock(text, dimmed, width);
  std::istringstream stream(rendered);
  std::string line;
  int wrapWidth = width - static_cast<int>(prefix.size());

  // For dimmed text, build a style prefix with dim ANSI codes that gets
  // reapplied to each continuation line after wrapping.
  std::string stylePrefix;
  if (dimmed) {
    stylePrefix = "\x1b[2m\x1b[38;2;160;160;180m";
  }

  while (std::getline(stream, line)) {
    if (wrapWidth > 0 && visualWidth(line) > wrapWidth) {
      auto wrapped = wrapLine(line, wrapWidth, stylePrefix);
      for (auto& wl : wrapped) {
        lines.push_back(prefix + wl);
      }
    } else {
      lines.push_back(prefix + line);
    }
  }
  if (lines.empty()) lines.push_back(prefix);
  return lines;
}

int countBlockLines(const std::string& text, int width, int prefixLen) {
  std::vector<std::string> lines;
  std::istringstream stream(text);
  std::string line;
  int count = 0;
  while (std::getline(stream, line)) {
    int effectiveWidth = width - prefixLen;
    if (effectiveWidth <= 0) effectiveWidth = 1;
    count += std::max(1, (static_cast<int>(line.size()) + effectiveWidth - 1) / effectiveWidth);
  }
  return std::max(1, count);
}

} // namespace

// ── AgentTextItem ──

void AgentTextItem::appendDelta(const std::string& delta) {
  accumulated_ += delta;
  touch();
}

void AgentTextItem::finalize() {
  finalized_ = true;
  touch();
}

std::vector<std::string> AgentTextItem::render(int width) const {
  return renderStreamingBlock(accumulated_, width, "", false);
}

int AgentTextItem::rowCount(int width) const {
  return countBlockLines(accumulated_, width, 0);
}

// ── AgentThinkingItem ──

void AgentThinkingItem::appendDelta(const std::string& delta) {
  accumulated_ += delta;
  touch();
}

void AgentThinkingItem::finalize() {
  finalized_ = true;
  touch();
}

std::vector<std::string> AgentThinkingItem::render(int width) const {
  return renderStreamingBlock(accumulated_, width, "", true);
}

int AgentThinkingItem::rowCount(int width) const {
  return countBlockLines(accumulated_, width, 0);
}

} // namespace firmius::tui2

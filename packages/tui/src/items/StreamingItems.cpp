#include "items/StreamingItems.hpp"
#include "Terminal.hpp"
#include "ThemeAnsi.hpp"
#include "ThemeManager.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <iomanip>
#include <sstream>

namespace firmius::tui {

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
    const auto& theme = ThemeManager::instance().currentTheme();
    stylePrefix = "\x1b[2m\x1b[38;2;" + std::to_string(theme.base.dim.r) + ";" +
                  std::to_string(theme.base.dim.g) + ";" +
                  std::to_string(theme.base.dim.b) + "m";
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

} // namespace

std::vector<std::string> renderMarkdownLines(const std::string& text, int width, bool dimmed) {
  return renderStreamingBlock(text, width, "", dimmed);
}

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
  constexpr int kLeftMargin = 2;
  constexpr int kRightMargin = 4;
  const int contentWidth = std::max(1, width - kLeftMargin - kRightMargin);

  auto lines = renderMarkdownLines(accumulated_, contentWidth, false);
  const std::string dot = theme_ansi::dim("\xe2\x80\xa2 "); // •
  const std::string firstPrefix = std::string(kLeftMargin, ' ') + dot;
  const std::string contPrefix = std::string(kLeftMargin, ' ') + std::string(ansi::visibleWidth(dot), ' ');

  for (size_t i = 0; i < lines.size(); ++i) {
    lines[i] = (i == 0 ? firstPrefix : contPrefix) + lines[i];
  }
  return lines;
}

int AgentTextItem::rowCount(int width) const {
  return static_cast<int>(render(width).size());
}

// ── AgentThinkingItem ──

void AgentThinkingItem::appendDelta(const std::string& delta) {
  if (startedAt_ == std::chrono::steady_clock::time_point{}) {
    startedAt_ = std::chrono::steady_clock::now();
  }
  // Used to briefly "fade in" the newest words.
  lastDeltaAt_ = std::chrono::steady_clock::now();
  lastDeltaText_ = delta;
  accumulated_ += delta;
  touch();
}

void AgentThinkingItem::finalize() {
  if (startedAt_ == std::chrono::steady_clock::time_point{}) {
    startedAt_ = std::chrono::steady_clock::now();
  }
  finishedAt_ = std::chrono::steady_clock::now();
  collapseStartedAt_ = finishedAt_;
  finalized_ = true;
  touch();
}

void AgentThinkingItem::setExpanded(bool expanded) {
  if (expanded_ == expanded) return;
  expanded_ = expanded;
  touch();
}

bool AgentThinkingItem::needsAnimationTick() const {
  if (!finalized_) return true; // pulsing dot + fade-in
  if (expanded_) return false;
  if (collapseStartedAt_ == std::chrono::steady_clock::time_point{}) return false;
  const auto now = std::chrono::steady_clock::now();
  if (now - collapseStartedAt_ < std::chrono::milliseconds(kCollapseMs)) return true;
  if (lastDeltaAt_ != std::chrono::steady_clock::time_point{} &&
      now - lastDeltaAt_ < std::chrono::milliseconds(500)) return true;
  return false;
}

namespace {

std::string formatSeconds(std::chrono::milliseconds ms) {
  const double seconds = static_cast<double>(ms.count()) / 1000.0;
  std::ostringstream out;
  out.setf(std::ios::fixed);
  out << std::setprecision(1) << seconds;
  return out.str();
}

std::string pulsingDot(std::uint64_t nowMs, bool active) {
  if (!active) {
    return theme_ansi::dim("\xe2\x80\xa2 "); // •
  }
  const bool bright = ((nowMs / 420ULL) % 2ULL) == 0ULL;
  if (bright) return ansi::bold(theme_ansi::accent("\xe2\x80\xa2 ")); // •
  return theme_ansi::dim("\xe2\x80\xa2 "); // •
}

std::uint64_t nowMsSteady() {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now().time_since_epoch())
          .count());
}

struct FadeRgb {
  int r;
  int g;
  int b;
};

FadeRgb mix(FadeRgb from, FadeRgb to, double t) {
  const double clamped = std::clamp(t, 0.0, 1.0);
  return {
      static_cast<int>(std::lround(from.r + (to.r - from.r) * clamped)),
      static_cast<int>(std::lround(from.g + (to.g - from.g) * clamped)),
      static_cast<int>(std::lround(from.b + (to.b - from.b) * clamped)),
  };
}

std::string replaceAll(std::string text, const std::string& from,
                       const std::string& to) {
  if (from.empty()) return text;
  size_t pos = 0;
  while ((pos = text.find(from, pos)) != std::string::npos) {
    text.replace(pos, from.size(), to);
    pos += to.size();
  }
  return text;
}

std::string retintDimAnsi(std::string text, const FadeRgb& color) {
  const auto& theme = ThemeManager::instance().currentTheme();
  const std::string target =
      "\x1b[38;2;" + std::to_string(color.r) + ";" + std::to_string(color.g) +
      ";" + std::to_string(color.b) + "m";
  const std::string themedDim =
      "\x1b[38;2;" + std::to_string(theme.base.dim.r) + ";" +
      std::to_string(theme.base.dim.g) + ";" +
      std::to_string(theme.base.dim.b) + "m";
  text = replaceAll(std::move(text), themedDim, target);
  text = replaceAll(std::move(text), "\x1b[38;2;160;160;180m", target);
  return text;
}

size_t commonPrefixBytes(const std::string& a, const std::string& b) {
  const size_t limit = std::min(a.size(), b.size());
  size_t i = 0;
  while (i < limit && a[i] == b[i]) ++i;
  return i;
}

size_t visibleByteOffset(const std::string& text, size_t visibleChars) {
  if (visibleChars == 0) return 0;
  size_t i = 0;
  size_t visible = 0;
  while (i < text.size()) {
    if (text[i] == '\033' && i + 1 < text.size() && text[i + 1] == '[') {
      i += 2;
      while (i < text.size() && text[i] != 'm') ++i;
      if (i < text.size()) ++i;
      continue;
    }

    unsigned char c = static_cast<unsigned char>(text[i]);
    size_t charLen = 1;
    if ((c & 0x80U) == 0U) {
      charLen = 1;
    } else if ((c & 0xE0U) == 0xC0U) {
      charLen = 2;
    } else if ((c & 0xF0U) == 0xE0U) {
      charLen = 3;
    } else if ((c & 0xF8U) == 0xF0U) {
      charLen = 4;
    }
    i += charLen;
    ++visible;
    if (visible >= visibleChars) return i;
  }
  return text.size();
}

} // namespace

std::vector<std::string> AgentThinkingItem::render(int width) const {
  const auto now = std::chrono::steady_clock::now();
  const auto started = (startedAt_ == std::chrono::steady_clock::time_point{}) ? now : startedAt_;
  const auto ended = finalized_ ? (finishedAt_ == std::chrono::steady_clock::time_point{} ? now : finishedAt_) : now;
  const auto dur = std::chrono::duration_cast<std::chrono::milliseconds>(ended - started);
  const std::string seconds = formatSeconds(dur);

  const bool active = !finalized_;
  const std::string header =
      pulsingDot(nowMsSteady(), active) +
      (finalized_ ? "Thought for " : "Thinking for ") + seconds + "s";

  // Render thinking markdown as dimmed text; then draw a tree-like prefix.
  constexpr int kIndent = 2;
  constexpr int kRightMargin = 4;
  constexpr int kPreviewLines = 4;
  const int innerWidth = std::max(1, width - kIndent - kRightMargin);
  auto rendered = renderMarkdownLines(accumulated_, innerWidth - 3, true); // 3 = "│  " / "└─ "

  int previewCount = static_cast<int>(rendered.size());
  if (!expanded_) previewCount = std::min(previewCount, kPreviewLines);
  int visibleCount = previewCount;

  if (finalized_ && !expanded_) {
    const auto collapseStart = (collapseStartedAt_ == std::chrono::steady_clock::time_point{}) ? ended : collapseStartedAt_;
    const auto dt = std::chrono::duration_cast<std::chrono::milliseconds>(now - collapseStart).count();
    if (dt >= 0) {
      const double t = std::clamp(static_cast<double>(dt) / static_cast<double>(kCollapseMs), 0.0, 1.0);
      const int target = 0;
      const int startCount = std::min(static_cast<int>(rendered.size()), kPreviewLines);
      visibleCount = static_cast<int>(std::round((1.0 - t) * startCount + t * target));
      visibleCount = std::max(0, std::min(visibleCount, startCount));
    }
  }

  const int startIndex = std::max(0, static_cast<int>(rendered.size()) - visibleCount);

  std::vector<std::string> out;
  out.push_back(std::string(kIndent, ' ') + header);

  const auto& theme = ThemeManager::instance().currentTheme();
  const FadeRgb bg{theme.base.bg.r, theme.base.bg.g, theme.base.bg.b};
  const FadeRgb dim{theme.base.dim.r, theme.base.dim.g, theme.base.dim.b};
  const auto fadeMs = lastDeltaAt_ == std::chrono::steady_clock::time_point{}
                          ? 1000LL
                          : std::chrono::duration_cast<std::chrono::milliseconds>(
                                now - lastDeltaAt_)
                                .count();
  const bool fadingIn = !finalized_ && fadeMs >= 0 && fadeMs < 500;
  const double fadeT =
      std::clamp(static_cast<double>(fadeMs) / 500.0, 0.0, 1.0);
  const FadeRgb fadeColor = mix(bg, dim, fadeT);

  std::vector<std::string> beforeRendered;
  if (fadingIn && !lastDeltaText_.empty() &&
      accumulated_.size() >= lastDeltaText_.size()) {
    beforeRendered = renderMarkdownLines(
        accumulated_.substr(0, accumulated_.size() - lastDeltaText_.size()),
        innerWidth - 3, true);
  }

  for (int i = 0; i < visibleCount; ++i) {
    const bool last = (i == visibleCount - 1);
    const std::string prefix = last ? "\xe2\x94\x94\xe2\x94\x80 "  // └─
                                    : "\xe2\x94\x82  ";          // │··
    std::string line = rendered[static_cast<std::size_t>(startIndex + i)];

    // When collapsed (default), make the earlier lines dimmer than the bottom.
    if (!expanded_ && visibleCount > 1) {
      const int distanceFromBottom = visibleCount - 1 - i;
      if (distanceFromBottom >= 2) {
        line = ansi::dim(line);
      } else if (distanceFromBottom == 1) {
        line = retintDimAnsi(std::move(line), mix(dim, bg, 0.20));
      }
    }

    if (fadingIn) {
      const int absIndex = startIndex + i;
      const int beforeCount = static_cast<int>(beforeRendered.size());
      if (beforeCount == 0) {
        line = retintDimAnsi(std::move(line), fadeColor);
      } else if (absIndex >= beforeCount - 1) {
        if (absIndex == beforeCount - 1) {
          const std::string& beforeLine =
              beforeRendered[static_cast<std::size_t>(beforeCount - 1)];
          const std::string beforePlain = ansi::strip(beforeLine);
          const std::string currentPlain = ansi::strip(line);
          const size_t keepVisible = commonPrefixBytes(beforePlain, currentPlain);
          const size_t keepBytes = visibleByteOffset(line, keepVisible);
          if (keepBytes < line.size()) {
            line = line.substr(0, keepBytes) +
                   retintDimAnsi(line.substr(keepBytes), fadeColor);
          }
        } else {
          line = retintDimAnsi(std::move(line), fadeColor);
        }
      }
    }

    out.push_back(std::string(kIndent, ' ') + theme_ansi::dim(prefix) + line);
  }
  return out;
}

int AgentThinkingItem::rowCount(int width) const {
  return static_cast<int>(render(width).size());
}

} // namespace firmius::tui

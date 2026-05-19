#include "DiffRenderer.hpp"
#include "SyntaxHighlighter.hpp"
#include "Terminal.hpp"
#include "ThemeAnsi.hpp"
#include "ThemeManager.hpp"

#include <algorithm>
#include <regex>
#include <sstream>
#include <string>

namespace firmius::tui {

namespace {

constexpr int kMaxContextLines = 5;

struct DiffLine {
  char type = ' ';     ///< '+', '-', ' '
  std::string content; ///< Line content (without leading prefix char)
  int oldLine = 0;     ///< 0 == unknown / not applicable
  int newLine = 0;
};

struct ParsedHunk {
  std::string header;   ///< Anchor description (empty for real hunks)
  bool hasRealNumbers = false;
  int oldStart = 0;     ///< 1-indexed; only meaningful when hasRealNumbers
  int newStart = 0;
  std::vector<DiffLine> lines;
};

struct ParsedFile {
  std::string path;
  std::vector<ParsedHunk> hunks;
};

// Pad an ANSI-colored string to exactly `targetWidth` visible columns,
// repeating `pad` to fill. Useful for full-width bg rows.
std::string padToWidth(const std::string& text, int targetWidth,
                       char pad = ' ') {
  const int visible = ansi::visibleWidth(text);
  if (visible >= targetWidth) return text;
  return text + std::string(targetWidth - visible, pad);
}

// Truncate (visibly) — preserves ANSI sequences and inserts a soft ellipsis.
// Conservative: aborts cleanly inside escape sequences.
std::string truncateToWidth(const std::string& text, int targetWidth) {
  if (ansi::visibleWidth(text) <= targetWidth) return text;
  std::string out;
  int visible = 0;
  bool inEscape = false;
  for (size_t i = 0; i < text.size(); ++i) {
    if (text[i] == '\x1b') {
      inEscape = true;
      out += text[i];
      continue;
    }
    if (inEscape) {
      out += text[i];
      if ((text[i] >= 'A' && text[i] <= 'Z') ||
          (text[i] >= 'a' && text[i] <= 'z')) {
        inEscape = false;
      }
      continue;
    }
    // UTF-8 continuation bytes don't bump the visible counter.
    if ((static_cast<unsigned char>(text[i]) & 0xC0) == 0x80) {
      out += text[i];
      continue;
    }
    if (visible >= targetWidth - 1) {
      // Append ellipsis and stop (unclosed escapes will be reset by caller).
      out += "\xe2\x80\xa6";
      break;
    }
    out += text[i];
    ++visible;
  }
  return out;
}

// Parse a unified-diff hunk header like `@@ -312,5 +315,7 @@` or
// `@@ -1 +1 @@` (count optional). Returns false when not a unified header.
bool parseUnifiedHunkHeader(const std::string& line, int& oldStart,
                            int& newStart) {
  static const std::regex re(
      R"(^@@\s+-(\d+)(?:,\d+)?\s+\+(\d+)(?:,\d+)?\s+@@.*)");
  std::smatch m;
  if (!std::regex_match(line, m, re)) return false;
  try {
    oldStart = std::stoi(m[1].str());
    newStart = std::stoi(m[2].str());
    return true;
  } catch (...) {
    return false;
  }
}

// Parse the firmius anchor header `@@ <description> @@`. Returns the inner
// description text on match.
bool parseAnchorHunkHeader(const std::string& line, std::string& description) {
  if (line.size() < 6) return false;
  if (line.rfind("@@ ", 0) != 0) return false;
  const auto end = line.rfind(" @@");
  if (end == std::string::npos || end <= 3) return false;
  description = line.substr(3, end - 3);
  return true;
}

std::vector<ParsedFile> parseDiff(const std::string& diff) {
  std::vector<ParsedFile> files;
  std::istringstream stream(diff);
  std::string line;

  ParsedFile* currentFile = nullptr;
  ParsedHunk* currentHunk = nullptr;

  auto ensureFile = [&]() -> ParsedFile& {
    if (!currentFile) {
      files.push_back({});
      currentFile = &files.back();
    }
    return *currentFile;
  };

  while (std::getline(stream, line)) {
    if (line.rfind("diff --git", 0) == 0) {
      // Skip — the +++/--- pair carries the path.
      continue;
    }
    if (line.rfind("--- ", 0) == 0) {
      // Old-file header. Some producers emit `--- /dev/null` for new files.
      // The path comes from `+++` so just close out the previous file.
      if (currentFile && !currentFile->hunks.empty()) {
        currentFile = nullptr;
        currentHunk = nullptr;
      }
      continue;
    }
    if (line.rfind("+++ ", 0) == 0) {
      files.push_back({});
      currentFile = &files.back();
      currentHunk = nullptr;
      std::string p = line.substr(4);
      if (p.rfind("a/", 0) == 0 || p.rfind("b/", 0) == 0) p = p.substr(2);
      currentFile->path = p;
      continue;
    }

    int oldStart = 0, newStart = 0;
    std::string desc;
    if (parseUnifiedHunkHeader(line, oldStart, newStart)) {
      auto& f = ensureFile();
      f.hunks.push_back({});
      currentHunk = &f.hunks.back();
      currentHunk->hasRealNumbers = true;
      currentHunk->oldStart = oldStart;
      currentHunk->newStart = newStart;
      // Capture any trailing function-context after `@@`.
      const auto trailing = line.find("@@", 2);
      if (trailing != std::string::npos && trailing + 2 < line.size()) {
        std::string ctx = line.substr(trailing + 2);
        // Trim leading spaces.
        size_t i = 0;
        while (i < ctx.size() && ctx[i] == ' ') ++i;
        if (i < ctx.size()) currentHunk->header = ctx.substr(i);
      }
      continue;
    }
    if (parseAnchorHunkHeader(line, desc)) {
      auto& f = ensureFile();
      f.hunks.push_back({});
      currentHunk = &f.hunks.back();
      currentHunk->hasRealNumbers = false;
      currentHunk->header = desc;
      continue;
    }

    if (line.empty()) {
      // Empty body lines inside a hunk are just blank context.
      if (currentHunk) {
        currentHunk->lines.push_back({' ', "", 0, 0});
      }
      continue;
    }

    if (!currentHunk) {
      // Stray content with no hunk — synthesize an unnamed hunk so we still
      // render something. Treat as anchor-style with no header.
      auto& f = ensureFile();
      f.hunks.push_back({});
      currentHunk = &f.hunks.back();
      currentHunk->hasRealNumbers = false;
    }

    char prefix = line[0];
    std::string content = line.size() > 1 ? line.substr(1) : "";
    DiffLine dl;
    dl.content = std::move(content);
    if (prefix == '+') {
      dl.type = '+';
    } else if (prefix == '-') {
      dl.type = '-';
    } else if (prefix == ' ') {
      dl.type = ' ';
    } else if (prefix == '\\') {
      // "No newline at end of file" sentinel — silently drop.
      continue;
    } else {
      // Unknown prefix; treat the entire line as context.
      dl.type = ' ';
      dl.content = line;
    }
    currentHunk->lines.push_back(std::move(dl));
  }

  // Assign line numbers within each hunk by walking forward from the header.
  for (auto& file : files) {
    for (auto& hunk : file.hunks) {
      if (!hunk.hasRealNumbers) {
        // Anchor-style: no real line numbers available. Leave them as 0; the
        // renderer will print blanks in the gutter.
        continue;
      }
      int oldNo = hunk.oldStart;
      int newNo = hunk.newStart;
      for (auto& dl : hunk.lines) {
        if (dl.type == ' ') {
          dl.oldLine = oldNo++;
          dl.newLine = newNo++;
        } else if (dl.type == '-') {
          dl.oldLine = oldNo++;
          dl.newLine = 0;
        } else if (dl.type == '+') {
          dl.oldLine = 0;
          dl.newLine = newNo++;
        }
      }
    }
  }

  return files;
}

// Compute the column width needed to display the largest line number we'll
// ever print for this file (across all hunks).
int gutterWidthFor(const ParsedFile& file) {
  int largest = 0;
  for (const auto& hunk : file.hunks) {
    if (!hunk.hasRealNumbers) continue;
    for (const auto& dl : hunk.lines) {
      largest = std::max(largest, dl.oldLine);
      largest = std::max(largest, dl.newLine);
    }
  }
  if (largest <= 0) return 0; // anchor-style → no gutter
  int w = 1;
  for (int v = largest; v >= 10; v /= 10) ++w;
  return std::max(w, 3);
}

std::string padNumber(int n, int width) {
  if (n <= 0) return std::string(width, ' ');
  std::string s = std::to_string(n);
  if (static_cast<int>(s.size()) < width) {
    s = std::string(width - s.size(), ' ') + s;
  }
  return s;
}

// Render a single body row, full-width, with the appropriate background
// color for its diff type. Syntax tokens are colored on top.
std::string renderBodyRow(const DiffLine& dl, int width, int gutter,
                          const std::string& language,
                          const ThemeSpec& theme) {
  // Pick bg + sigil + sigil color.
  ThemeRgb bg = theme.diff.contextBg;
  ThemeRgb sigilFg = theme.base.dim;
  ThemeRgb fallbackFg = theme.base.fg;
  char sigilChar = ' ';
  if (dl.type == '+') {
    bg = theme.diff.addBg;
    sigilFg = theme.statusBar.context.low;
    sigilChar = '+';
  } else if (dl.type == '-') {
    bg = theme.diff.removeBg;
    sigilFg = theme.statusBar.error.normal.fg;
    sigilChar = '-';
  }

  // ── Gutter ──
  std::string gutterText;
  if (gutter > 0) {
    gutterText = padNumber(dl.oldLine, gutter) + " " +
                 padNumber(dl.newLine, gutter) + " ";
  }

  // ── Sigil ──
  std::string sigilStr(1, sigilChar);
  sigilStr += " ";

  // ── Content (syntax-highlighted) ──
  std::string content;
  if (!language.empty() &&
      SyntaxHighlighter::instance().hasGrammar(language)) {
    content = SyntaxHighlighter::instance().highlightLine(dl.content, language,
                                                         fallbackFg);
  } else {
    content = ansi::fgRgb(fallbackFg.r, fallbackFg.g, fallbackFg.b, dl.content);
  }

  // Compose the inner row (no bg yet).
  const std::string gutterColored =
      ansi::fgRgb(theme.diff.gutterFg.r, theme.diff.gutterFg.g,
                  theme.diff.gutterFg.b, gutterText);
  const std::string sigilColored =
      ansi::fgRgb(sigilFg.r, sigilFg.g, sigilFg.b, sigilStr);

  std::string inner = gutterColored + sigilColored + content;

  // Truncate / pad to width before applying bg so the bg fills exactly the
  // available row.
  const int innerWidth = ansi::visibleWidth(inner);
  if (innerWidth > width) {
    inner = truncateToWidth(inner, width);
  } else if (innerWidth < width) {
    inner = padToWidth(inner, width);
  }

  // Apply bg color to the whole row.
  return ansi::bgRgb(bg.r, bg.g, bg.b, inner);
}

std::string renderHunkHeader(const ParsedHunk& hunk, int width,
                             const ThemeSpec& theme) {
  std::string body;
  if (hunk.hasRealNumbers) {
    body = "@@ -" + std::to_string(hunk.oldStart) + " +" +
           std::to_string(hunk.newStart) + " @@";
    if (!hunk.header.empty()) body += " " + hunk.header;
  } else if (!hunk.header.empty()) {
    body = "@@ " + hunk.header + " @@";
  } else {
    body = "@@ ... @@";
  }
  body = "  " + body;

  std::string colored = ansi::fgRgb(theme.diff.gutterFg.r,
                                    theme.diff.gutterFg.g,
                                    theme.diff.gutterFg.b, body);
  if (ansi::visibleWidth(colored) > width) {
    colored = truncateToWidth(colored, width);
  } else {
    colored = padToWidth(colored, width);
  }
  return ansi::bgRgb(theme.diff.headerBg.r, theme.diff.headerBg.g,
                     theme.diff.headerBg.b, colored);
}

// Render one hunk's body rows, collapsing long context runs.
std::vector<std::string> renderHunk(const ParsedHunk& hunk, int width,
                                    int gutter, const std::string& language,
                                    const ThemeSpec& theme) {
  std::vector<std::string> rows;

  int contextStreak = 0;
  int pendingOmitted = 0;

  for (const auto& dl : hunk.lines) {
    if (dl.type == ' ') {
      contextStreak++;
      if (contextStreak > kMaxContextLines) {
        ++pendingOmitted;
        continue;
      }
    } else {
      if (pendingOmitted > 0) {
        std::string gap = "  ... (" + std::to_string(pendingOmitted) +
                          " lines omitted) ...";
        std::string colored = ansi::dim(
            ansi::fgRgb(theme.diff.gutterFg.r, theme.diff.gutterFg.g,
                        theme.diff.gutterFg.b, gap));
        colored = padToWidth(colored, width);
        colored = ansi::bgRgb(theme.diff.contextBg.r, theme.diff.contextBg.g,
                              theme.diff.contextBg.b, colored);
        rows.push_back(colored);
        pendingOmitted = 0;
      }
      contextStreak = 0;
    }

    rows.push_back(renderBodyRow(dl, width, gutter, language, theme));
  }

  if (pendingOmitted > 0) {
    std::string gap = "  ... (" + std::to_string(pendingOmitted) +
                      " lines omitted) ...";
    std::string colored = ansi::dim(
        ansi::fgRgb(theme.diff.gutterFg.r, theme.diff.gutterFg.g,
                    theme.diff.gutterFg.b, gap));
    colored = padToWidth(colored, width);
    colored = ansi::bgRgb(theme.diff.contextBg.r, theme.diff.contextBg.g,
                          theme.diff.contextBg.b, colored);
    rows.push_back(colored);
  }

  return rows;
}

} // namespace

std::vector<std::string> DiffRenderer::render(const std::string& diffPreview,
                                              int width,
                                              const std::string& sourcePath) {
  if (width <= 0) width = 80;
  std::vector<std::string> result;

  if (diffPreview.empty()) {
    result.push_back(theme_ansi::dim("  (no diff)"));
    return result;
  }

  const auto& theme = ThemeManager::instance().currentTheme();
  const auto files = parseDiff(diffPreview);

  int totalAdded = 0;
  int totalRemoved = 0;
  for (const auto& f : files) {
    for (const auto& h : f.hunks) {
      for (const auto& l : h.lines) {
        if (l.type == '+') ++totalAdded;
        else if (l.type == '-') ++totalRemoved;
      }
    }
  }

  for (size_t fi = 0; fi < files.size(); ++fi) {
    const auto& file = files[fi];
    if (file.hunks.empty()) continue;

    // Decide language for this file. If the producer didn't include a
    // `+++ b/path` header (anchor-only diffs from FileEditTool), fall back
    // to `sourcePath` provided by the caller.
    const std::string detect_path = !file.path.empty() ? file.path : sourcePath;
    const std::string language =
        detect_path.empty()
            ? std::string{}
            : SyntaxHighlighter::instance().detectLanguage(detect_path);

    // File header pill — only when we actually know a path.
    if (!file.path.empty()) {
      std::string title = "  " + file.path;
      std::string colored = ansi::bold(theme_ansi::accent(title));
      colored = padToWidth(colored, width);
      result.push_back(colored);
    }

    const int gutter = gutterWidthFor(file);

    for (size_t hi = 0; hi < file.hunks.size(); ++hi) {
      const auto& hunk = file.hunks[hi];
      // Header row for the hunk.
      result.push_back(renderHunkHeader(hunk, width, theme));
      auto bodyRows = renderHunk(hunk, width, gutter, language, theme);
      result.insert(result.end(), bodyRows.begin(), bodyRows.end());
    }
  }

  if (totalAdded > 0 || totalRemoved > 0) {
    std::string footer = "  ";
    if (totalAdded > 0) footer += theme_ansi::success("+" + std::to_string(totalAdded));
    if (totalAdded > 0 && totalRemoved > 0) footer += " ";
    if (totalRemoved > 0) footer += theme_ansi::error("-" + std::to_string(totalRemoved));
    result.push_back(ansi::dim(footer));
  }

  return result;
}

} // namespace firmius::tui

#include "DiffRenderer.hpp"
#include "Terminal.hpp"
#include "ThemeManager.hpp"
#include "ThemeAnsi.hpp"

#include <algorithm>
#include <sstream>
#include <string>

namespace firmius::tui2 {

namespace {

constexpr int kMaxContextLines = 5;

struct DiffLine {
  char type = ' ';  // '+', '-', ' '
  std::string content;
};

std::vector<std::string> renderDiffLines(const std::vector<DiffLine>& lines, int /*width*/) {
  const auto& theme = ThemeManager::instance().currentTheme();
  std::vector<std::string> result;
  result.reserve(lines.size());

  int oldLine = 0;
  int newLine = 0;
  int contextStreak = 0;
  int pendingOmitted = 0;

  for (size_t i = 0; i < lines.size(); ++i) {
    const auto& dl = lines[i];

    if (dl.type == ' ') {
      oldLine++;
      newLine++;
      contextStreak++;

      if (contextStreak > kMaxContextLines) {
        // Count how many we're skipping
        if (pendingOmitted == 0) {
          // First line exceeding threshold — emit the gap marker later
          pendingOmitted = 1;
        } else {
          pendingOmitted++;
        }
        continue;
      }
    } else {
      // Flush any pending gap
      if (pendingOmitted > 0) {
        std::string gap = theme_ansi::dim(
            "  ... (" + std::to_string(pendingOmitted) + " lines omitted) ...");
        result.push_back(gap);
        pendingOmitted = 0;
      }
      contextStreak = 0;
    }

    // Format line numbers — fixed-width columns for alignment
    auto padNum = [](int n, int width) {
      std::string s = std::to_string(n);
      while (static_cast<int>(s.size()) < width) s = " " + s;
      return s;
    };
    std::string lineNum;
    if (dl.type == '+') {
      newLine++;
      lineNum = theme_ansi::dim("   " + padNum(newLine, 4) + "  ");
    } else if (dl.type == '-') {
      oldLine++;
      lineNum = theme_ansi::dim(padNum(oldLine, 4) + "      ");
    } else {
      lineNum = theme_ansi::dim(padNum(oldLine, 4) + " " + padNum(newLine, 4) + " ");
    }

    std::string prefix;
    std::string styled;
    if (dl.type == '+') {
      prefix = theme_ansi::success("+");
      styled = ansi::bgRgb(theme.statusBar.streaming.normal.bg.r,
                           theme.statusBar.streaming.normal.bg.g,
                           theme.statusBar.streaming.normal.bg.b,
                           ansi::fgRgb(theme.statusBar.streaming.normal.fg.r,
                                       theme.statusBar.streaming.normal.fg.g,
                                       theme.statusBar.streaming.normal.fg.b,
                                       dl.content));
    } else if (dl.type == '-') {
      prefix = theme_ansi::error("-");
      styled = ansi::bgRgb(theme.statusBar.error.normal.bg.r,
                           theme.statusBar.error.normal.bg.g,
                           theme.statusBar.error.normal.bg.b,
                           ansi::fgRgb(theme.statusBar.error.normal.fg.r,
                                       theme.statusBar.error.normal.fg.g,
                                       theme.statusBar.error.normal.fg.b,
                                       dl.content));
    } else {
      prefix = theme_ansi::dim(" ");
      styled = theme_ansi::dim(dl.content);
    }

    result.push_back("  " + lineNum + prefix + styled);
  }

  // Flush trailing gap
  if (pendingOmitted > 0) {
    std::string gap = theme_ansi::dim(
        "  ... (" + std::to_string(pendingOmitted) + " lines omitted) ...");
    result.push_back(gap);
  }

  return result;
}

} // namespace

std::vector<std::string> DiffRenderer::render(const std::string& diffPreview, int width) {
  std::vector<std::string> result;

  if (diffPreview.empty()) {
    result.push_back(theme_ansi::dim("  (no diff)"));
    return result;
  }

  std::istringstream stream(diffPreview);
  std::string line;

  std::string currentFile;
  std::vector<DiffLine> currentLines;
  int totalAdded = 0;
  int totalRemoved = 0;

  auto flushFile = [&]() {
    if (!currentLines.empty()) {
      if (!currentFile.empty()) {
        result.push_back(ansi::bold(theme_ansi::accent("  " + currentFile)));
      }
      auto rendered = renderDiffLines(currentLines, width);
      result.insert(result.end(), rendered.begin(), rendered.end());
      currentLines.clear();
    }
  };

  while (std::getline(stream, line)) {
    if (line.rfind("--- ", 0) == 0) {
      // File header — old file
      continue;
    }
    if (line.rfind("+++ ", 0) == 0) {
      flushFile();
      currentFile = line.substr(4);
      continue;
    }
    if (line.rfind("@@ ", 0) == 0) {
      // Hunk header — flush previous lines, render header
      flushFile();
      result.push_back(theme_ansi::dim("  " + line));
      continue;
    }
    if (!line.empty()) {
      char type = line[0];
      std::string content = line.size() > 1 ? line.substr(1) : "";
      if (type == '+') {
        totalAdded++;
        currentLines.push_back({'+', content});
      } else if (type == '-') {
        totalRemoved++;
        currentLines.push_back({'-', content});
      } else {
        currentLines.push_back({' ', content});
      }
    }
  }

  flushFile();

  // Footer with summary
  if (totalAdded > 0 || totalRemoved > 0) {
    std::string footer = "  ";
    if (totalAdded > 0) {
      footer += theme_ansi::success("+" + std::to_string(totalAdded));
    }
    if (totalAdded > 0 && totalRemoved > 0) {
      footer += " ";
    }
    if (totalRemoved > 0) {
      footer += theme_ansi::error("-" + std::to_string(totalRemoved));
    }
    result.push_back(ansi::dim(footer));
  }

  return result;
}

} // namespace firmius::tui2

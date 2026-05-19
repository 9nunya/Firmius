#include "AnsiOutputParser.hpp"
#include "Terminal.hpp"
#include "ThemeAnsi.hpp"

#include <algorithm>
#include <sstream>

namespace firmius::tui {

namespace {

// Count visible characters in a string (skip ANSI escape sequences).
int visibleLength(const std::string& s) {
  int len = 0;
  bool inEscape = false;
  for (size_t i = 0; i < s.size(); ++i) {
    if (s[i] == '\x1b') {
      inEscape = true;
      continue;
    }
    if (inEscape) {
      if ((s[i] >= 'A' && s[i] <= 'Z') || (s[i] >= 'a' && s[i] <= 'z')) {
        inEscape = false;
      }
      continue;
    }
    len++;
  }
  return len;
}

// Split a line at the visible width boundary, preserving ANSI codes.
std::vector<std::string> wrapAnsiLine(const std::string& line, int maxWidth) {
  if (visibleLength(line) <= maxWidth) {
    return {line};
  }

  std::vector<std::string> result;
  std::string current;
  int currentVisLen = 0;
  bool inEscape = false;
  std::string currentEscape;

  for (size_t i = 0; i < line.size(); ++i) {
    if (line[i] == '\x1b') {
      inEscape = true;
      currentEscape.clear();
      currentEscape += line[i];
      continue;
    }
    if (inEscape) {
      currentEscape += line[i];
      if ((line[i] >= 'A' && line[i] <= 'Z') || (line[i] >= 'a' && line[i] <= 'z')) {
        inEscape = false;
        current += currentEscape;
        currentEscape.clear();
      }
      continue;
    }

    if (currentVisLen >= maxWidth) {
      result.push_back(current);
      current.clear();
      currentVisLen = 0;
    }
    current += line[i];
    currentVisLen++;
  }

  if (!current.empty()) {
    result.push_back(current);
  }

  if (result.empty()) {
    result.push_back("");
  }

  return result;
}

} // namespace

std::vector<std::string> AnsiOutputParser::toLines(const std::string& raw, int width,
                                                    int maxLines) {
  std::vector<std::string> result;

  if (raw.empty()) {
    result.push_back(theme_ansi::dim("  \xe2\x94\x82 (no output)"));
    return result;
  }

  std::string barPrefix = theme_ansi::dim("  \xe2\x94\x82 ");
  int prefixVisibleLen = 3; // "  │ "
  int contentWidth = width - prefixVisibleLen;
  if (contentWidth < 10) contentWidth = 10;

  // Split raw into lines
  std::istringstream stream(raw);
  std::string line;
  std::vector<std::string> allLines;

  while (std::getline(stream, line)) {
    // Remove trailing \r
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    auto wrapped = wrapAnsiLine(line, contentWidth);
    for (auto& wl : wrapped) {
      allLines.push_back(barPrefix + wl);
    }
  }

  if (allLines.empty()) {
    result.push_back(theme_ansi::dim("  \xe2\x94\x82 (no output)"));
    return result;
  }

  // If maxLines > 0 and we exceed it, take the LAST maxLines lines
  if (maxLines > 0 && static_cast<int>(allLines.size()) > maxLines) {
    int omitted = static_cast<int>(allLines.size()) - maxLines;
    result.push_back(theme_ansi::dim(
        "  ... " + std::to_string(omitted) + " earlier lines ..."));
    result.insert(result.end(),
                  allLines.end() - maxLines,
                  allLines.end());
  } else {
    result = std::move(allLines);
  }

  return result;
}

} // namespace firmius::tui

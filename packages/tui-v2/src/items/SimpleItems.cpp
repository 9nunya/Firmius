#include "items/SimpleItems.hpp"
#include "Terminal.hpp"

#include <algorithm>
#include <sstream>

namespace firmius::tui2 {

namespace {

std::vector<std::string> wrapText(const std::string& text, int width, const std::string& prefix) {
  std::vector<std::string> lines;
  std::istringstream stream(text);
  std::string line;
  while (std::getline(stream, line)) {
    if (static_cast<int>(prefix.size() + line.size()) <= width) {
      lines.push_back(prefix + line);
    } else {
      // Simple word wrap
      std::string current = prefix;
      std::istringstream words(line);
      std::string word;
      bool first = true;
      while (words >> word) {
        if (!first && static_cast<int>(current.size() + 1 + word.size()) > width) {
          lines.push_back(current);
          current = prefix + word;
        } else {
          if (!first) current += " ";
          current += word;
        }
        first = false;
      }
      if (current.size() > prefix.size()) {
        lines.push_back(current);
      }
    }
  }
  if (lines.empty()) lines.push_back(prefix);
  return lines;
}

int countWrappedLines(const std::string& text, int width, int prefixLen) {
  int count = 0;
  std::istringstream stream(text);
  std::string line;
  while (std::getline(stream, line)) {
    int effectiveWidth = width - prefixLen;
    if (effectiveWidth <= 0) effectiveWidth = 1;
    int lineLen = static_cast<int>(line.size());
    count += std::max(1, (lineLen + effectiveWidth - 1) / effectiveWidth);
  }
  return std::max(1, count);
}

} // namespace

// ── UserMessageItem ──

UserMessageItem::UserMessageItem(std::string text)
    : text_(std::move(text)) {
  // Immutable — mark clean after first render
}

std::vector<std::string> UserMessageItem::render(int width) const {
  return wrapText(text_, width, ansi::bold(ansi::fgRgb(100, 140, 220, "> ")));
}

int UserMessageItem::rowCount(int width) const {
  return countWrappedLines(text_, width, 2);
}

// ── ErrorMessageItem ──

ErrorMessageItem::ErrorMessageItem(std::string text)
    : text_(std::move(text)) {}

std::vector<std::string> ErrorMessageItem::render(int width) const {
  return wrapText(text_, width, ansi::fgRgb(220, 80, 80, "  ! "));
}

int ErrorMessageItem::rowCount(int width) const {
  return countWrappedLines(text_, width, 4);
}

// ── SystemNoticeItem ──

SystemNoticeItem::SystemNoticeItem(std::string text)
    : text_(std::move(text)) {}

std::vector<std::string> SystemNoticeItem::render(int width) const {
  return wrapText(text_, width, ansi::dim(ansi::fgRgb(120, 120, 140, "  ")));
}

int SystemNoticeItem::rowCount(int width) const {
  return countWrappedLines(text_, width, 2);
}

} // namespace firmius::tui2

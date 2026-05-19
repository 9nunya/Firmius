#include "items/SimpleItems.hpp"
#include "items/StreamingItems.hpp"
#include "Terminal.hpp"
#include "ThemeAnsi.hpp"
#include "ThemeManager.hpp"

#include <algorithm>
#include <sstream>

namespace firmius::tui {

namespace {

std::string cardFillLine(int totalWidth, const ThemeRgb& bg, const ThemeRgb& fg,
                         const std::string& inner) {
  const int innerWidth = std::max(0, totalWidth);
  std::string content = ansi::fitToWidth(inner, innerWidth);
  return ansi::bgRgb(bg.r, bg.g, bg.b,
                     ansi::fgRgb(fg.r, fg.g, fg.b, content));
}

std::vector<std::string> wrapText(const std::string& text, int width, const std::string& prefix) {
  std::vector<std::string> lines;
  std::istringstream stream(text);
  std::string line;
  while (std::getline(stream, line)) {
    if (static_cast<int>(prefix.size() + line.size()) <= width) {
      lines.push_back(prefix + line);
    } else {
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

UserMessageItem::UserMessageItem(std::string text, std::string agentId,
                                 std::string messageId, bool queued)
    : text_(std::move(text)),
      agentId_(std::move(agentId)),
      messageId_(std::move(messageId)),
      queued_(queued) {
  // Immutable — mark clean after first render
}

std::vector<std::string> UserMessageItem::render(int width) const {
  const auto& theme = ThemeManager::instance().currentTheme();
  constexpr int kOuterMargin = 2;
  constexpr int kInnerPad = 2;

  const int cardWidth = std::max(0, width);
  if (cardWidth <= kInnerPad * 2 + 4) {
    auto lines = renderMarkdownLines(text_, std::max(1, width - kOuterMargin * 2), false);
    if (queued_) {
      for (auto& line : lines) line = ansi::italic(theme_ansi::dim(line));
    }
    std::vector<std::string> out;
    out.push_back(cardFillLine(cardWidth, theme.chat.userBg, theme.chat.userFg,
                               std::string(static_cast<size_t>(cardWidth), ' ')));
    for (const auto& rawLine : lines) {
      const std::string padded =
          ansi::fitToWidth(rawLine, std::max(1, width - kOuterMargin * 2));
      const std::string inner = std::string(kOuterMargin, ' ') + padded +
                                std::string(kOuterMargin, ' ');
      out.push_back(cardFillLine(cardWidth, theme.chat.userBg, theme.chat.userFg, inner));
    }
    out.push_back(cardFillLine(cardWidth, theme.chat.userBg, theme.chat.userFg,
                               std::string(static_cast<size_t>(cardWidth), ' ')));
    return out;
  }

  const int contentWidth = std::max(1, cardWidth - kOuterMargin * 2 - kInnerPad * 2);
  auto contentLines = renderMarkdownLines(text_, contentWidth, false);
  if (queued_) {
    for (auto& line : contentLines) line = ansi::italic(theme_ansi::dim(line));
  }

  std::vector<std::string> out;
  out.push_back(cardFillLine(cardWidth, theme.chat.userBg, theme.chat.userFg,
                             std::string(static_cast<size_t>(cardWidth), ' ')));
  for (const auto& rawLine : contentLines) {
    const std::string padded = ansi::fitToWidth(rawLine, contentWidth);
    const std::string inner = std::string(kOuterMargin, ' ') +
                              std::string(kInnerPad, ' ') + padded +
                              std::string(kInnerPad, ' ') +
                              std::string(kOuterMargin, ' ');
    out.push_back(cardFillLine(cardWidth, theme.chat.userBg, theme.chat.userFg, inner));
  }
  out.push_back(cardFillLine(cardWidth, theme.chat.userBg, theme.chat.userFg,
                             std::string(static_cast<size_t>(cardWidth), ' ')));
  return out;
}

int UserMessageItem::rowCount(int width) const {
  return static_cast<int>(render(width).size());
}

void UserMessageItem::setQueued(bool queued) {
  if (queued_ == queued) {
    return;
  }
  queued_ = queued;
  touch();
}

// ── ErrorMessageItem ──

ErrorMessageItem::ErrorMessageItem(std::string text)
    : text_(std::move(text)) {}

std::vector<std::string> ErrorMessageItem::render(int width) const {
  return wrapText(text_, width, theme_ansi::error("  ! "));
}

int ErrorMessageItem::rowCount(int width) const {
  return countWrappedLines(text_, width, 4);
}

// ── SystemNoticeItem ──

SystemNoticeItem::SystemNoticeItem(std::string text)
    : text_(std::move(text)) {}

std::vector<std::string> SystemNoticeItem::render(int width) const {
  return wrapText(text_, width, theme_ansi::dim("  "));
}

int SystemNoticeItem::rowCount(int width) const {
  return countWrappedLines(text_, width, 2);
}

} // namespace firmius::tui

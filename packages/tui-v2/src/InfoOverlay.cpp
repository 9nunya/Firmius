#include "InfoOverlay.hpp"
#include "Terminal.hpp"
#include "ThemeAnsi.hpp"
#include "ThemeAnsi.hpp"

#include <algorithm>

namespace firmius::tui2 {

void InfoOverlay::setContent(std::vector<std::string> lines) {
  content_ = std::move(lines);
  scrollOffset_ = 0;
}

void InfoOverlay::open() {
  isOpen_ = true;
  scrollOffset_ = 0;
}

void InfoOverlay::close() {
  isOpen_ = false;
  content_.clear();
  scrollOffset_ = 0;
}

int InfoOverlay::height(int width) const {
  if (!isActive()) return 0;
  int contentRows = std::min(static_cast<int>(content_.size()), maxVisible_);
  if (content_.empty()) contentRows = 0;
  return 1 + 1 + contentRows + 1;
  (void)width;
}

std::vector<std::string> InfoOverlay::render(int width) const {
  std::vector<std::string> lines;
  if (!isActive()) return lines;

  lines.push_back(theme_ansi::accent(
      ansi::bold(ansi::fitToWidth(" " + title_, width))));

  lines.push_back(theme_ansi::divider(width));

  int totalContent = static_cast<int>(content_.size());
  int visible = std::min(totalContent, maxVisible_);

  if (scrollOffset_ > totalContent - visible) {
    scrollOffset_ = std::max(0, totalContent - visible);
  }

  if (totalContent == 0) {
    lines.push_back(theme_ansi::dim(ansi::fitToWidth("  (empty)", width)));
  } else {
    for (int i = scrollOffset_; i < scrollOffset_ + visible && i < totalContent; ++i) {
      lines.push_back(ansi::fitToWidth(content_[i], width));
    }
  }

  std::string hints;
  if (totalContent > visible) {
    int pct = totalContent > 0
                  ? (scrollOffset_ + visible) * 100 / totalContent
                  : 100;
    hints = ansi::dim(" " + std::to_string(scrollOffset_ + 1) + "-" +
                      std::to_string(std::min(scrollOffset_ + visible, totalContent)) +
                      " of " + std::to_string(totalContent) + " (" +
                      std::to_string(pct) + "%)");
  }
  hints += ansi::dim("  \xe2\x86\x91\xe2\x86\x93 scroll") +
           ansi::dim(" \xe2\x94\x82 ") +
           ansi::dim("Esc close");
  lines.push_back(ansi::fitToWidth(hints, width));

  return lines;
}

bool InfoOverlay::handleInput(const std::string& key) {
  if (!isActive()) return false;

  if (key == "\x1b") {
    if (onDismiss_) onDismiss_();
    return true;
  }

  if (key == "\x1b[A") {
    if (scrollOffset_ > 0) --scrollOffset_;
    return true;
  }
  if (key == "\x1b[B") {
    int maxOffset = std::max(0, static_cast<int>(content_.size()) - maxVisible_);
    if (scrollOffset_ < maxOffset) ++scrollOffset_;
    return true;
  }

  if (key == "\x1b[5~") {
    scrollOffset_ = std::max(0, scrollOffset_ - maxVisible_);
    return true;
  }
  if (key == "\x1b[6~") {
    int maxOffset = std::max(0, static_cast<int>(content_.size()) - maxVisible_);
    scrollOffset_ = std::min(maxOffset, scrollOffset_ + maxVisible_);
    return true;
  }

  return false;
}

bool InfoOverlay::handleMouse(const MouseEvent& event,
                              int /*screenRow*/,
                              int /*screenCol*/) {
  if (!isActive()) return false;

  if (event.type == MouseEvent::Type::Scroll) {
    if (event.button == MouseEvent::Button::ScrollUp) {
      if (scrollOffset_ > 0) --scrollOffset_;
    } else if (event.button == MouseEvent::Button::ScrollDown) {
      int maxOffset = std::max(0, static_cast<int>(content_.size()) - maxVisible_);
      if (scrollOffset_ < maxOffset) ++scrollOffset_;
    }
    return true;
  }

  return false;
}

} // namespace firmius::tui2

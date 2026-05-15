#include "MenuList.hpp"
#include "Terminal.hpp"

#include <algorithm>

namespace firmius::tui2 {

void MenuList::moveUp() {
  if (selectedIndex_ > 0) --selectedIndex_;
}

void MenuList::moveDown() {
  if (selectedIndex_ < static_cast<int>(items_.size()) - 1) ++selectedIndex_;
}

void MenuList::selectCurrent() {
  if (selectedIndex_ >= 0 && selectedIndex_ < static_cast<int>(items_.size())) {
    if (onSelect_) onSelect_(items_[selectedIndex_]);
  }
}

void MenuList::dismiss() {
  if (onDismiss_) onDismiss_();
}

int MenuList::height(int /*width*/) const {
  if (!isActive()) return 0;

  int itemRows = std::min(static_cast<int>(items_.size()), maxVisible_);
  // Title (1) + separator (1) + items + hint (1).
  return 1 + 1 + itemRows + 1;
}

std::vector<std::string> MenuList::render(int width) const {
  std::vector<std::string> lines;
  if (!isActive()) return lines;

  // Title line.
  lines.push_back(ansi::fgRgb(120, 180, 255,
                  ansi::bold(ansi::fitToWidth(" " + title_, width))));

  // Separator.
  lines.push_back(ansi::fgRgb(60, 60, 80, std::string(width, '-')));

  // Visible window around the selected index.
  int totalItems = static_cast<int>(items_.size());
  int visible = std::min(totalItems, maxVisible_);
  int scrollOffset = 0;
  if (selectedIndex_ >= visible) {
    scrollOffset = selectedIndex_ - visible + 1;
  }
  scrollOffset = std::min(scrollOffset, totalItems - visible);
  if (scrollOffset < 0) scrollOffset = 0;

  for (int i = scrollOffset; i < scrollOffset + visible && i < totalItems; ++i) {
    const auto& item = items_[i];
    bool selected = (i == selectedIndex_);

    std::string cursor = selected ? ansi::fgRgb(120, 220, 120, "> ")
                                   : "  ";
    std::string label = item.label;
    if (item.marked) {
      label += " " + ansi::fgRgb(100, 200, 100, "✔");
    }

    std::string row = cursor + label;
    if (!item.detail.empty()) {
      row += "  " + ansi::dim(item.detail);
    }

    if (selected) {
      row = ansi::bgRgb(40, 40, 55, ansi::fitToWidth(row, width));
    } else {
      row = ansi::fitToWidth(row, width);
    }

    lines.push_back(row);
  }

  // Hint line.
  std::string hints = ansi::dim(" ↑↓ navigate") +
                      ansi::dim(" │ ") +
                      ansi::dim("Enter select") +
                      ansi::dim(" │ ") +
                      ansi::dim("Esc cancel");
  lines.push_back(ansi::fitToWidth(hints, width));

  return lines;
}

} // namespace firmius::tui2

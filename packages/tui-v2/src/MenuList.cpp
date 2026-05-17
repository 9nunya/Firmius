#include "MenuList.hpp"
#include "Terminal.hpp"
#include "ThemeAnsi.hpp"

#include <algorithm>
#include <cctype>
#include <sstream>

namespace firmius::tui2 {

namespace {

/// Split query into space-separated tags.
std::vector<std::string> splitTags(const std::string& query) {
  std::vector<std::string> tags;
  std::istringstream stream(query);
  std::string tag;
  while (stream >> tag) {
    if (!tag.empty()) tags.push_back(tag);
  }
  return tags;
}

/// Case-insensitive substring check.
bool containsCI(const std::string& haystack, const std::string& needle) {
  if (needle.empty()) return true;
  if (haystack.size() < needle.size()) return false;
  for (size_t i = 0; i <= haystack.size() - needle.size(); ++i) {
    bool match = true;
    for (size_t j = 0; j < needle.size(); ++j) {
      if (std::tolower(static_cast<unsigned char>(haystack[i + j])) !=
          std::tolower(static_cast<unsigned char>(needle[j]))) {
        match = false;
        break;
      }
    }
    if (match) return true;
  }
  return false;
}

/// Fuzzy score for a single tag against a string. Higher = better match.
/// Returns -1 if no match.
int fuzzyScore(const std::string& text, const std::string& tag) {
  if (tag.empty()) return 0;
  if (!containsCI(text, tag)) return -1;

  // Exact substring match at start = best.
  size_t pos = text.find(tag);
  if (pos != std::string::npos) {
    if (pos == 0) return 1000;
    return 500 - static_cast<int>(pos);
  }

  // Case-insensitive match.
  std::string lowerText = text;
  std::string lowerTag = tag;
  for (auto& c : lowerText) c = std::tolower(static_cast<unsigned char>(c));
  for (auto& c : lowerTag) c = std::tolower(static_cast<unsigned char>(c));

  pos = lowerText.find(lowerTag);
  if (pos == 0) return 900;
  if (pos != std::string::npos) return 400 - static_cast<int>(pos);

  return 100;
}

/// Combined score for an item against all tags. All tags must match.
int itemScore(const MenuList::Item& item, const std::vector<std::string>& tags) {
  if (tags.empty()) return 0;

  int totalScore = 0;
  for (const auto& tag : tags) {
    int labelScore = fuzzyScore(item.label, tag);
    int detailScore = fuzzyScore(item.detail, tag);
    int best = std::max(labelScore, detailScore);
    if (best < 0) return -1; // All tags must match.
    totalScore += best;
  }
  return totalScore;
}

} // namespace

void MenuList::setItems(std::vector<Item> items) {
  items_ = std::move(items);
  selectedIndex_ = 0;
  filteredDirty_ = true;
}

void MenuList::rebuildFiltered() const {
  if (!filteredDirty_) return;
  filteredDirty_ = false;
  filteredIndices_.clear();

  auto tags = splitTags(searchQuery_);

  if (tags.empty()) {
    // No filter — show everything.
    for (int i = 0; i < static_cast<int>(items_.size()); ++i) {
      filteredIndices_.push_back(i);
    }
    return;
  }

  // Score and filter.
  struct Scored { int index; int score; };
  std::vector<Scored> scored;
  for (int i = 0; i < static_cast<int>(items_.size()); ++i) {
    int s = itemScore(items_[i], tags);
    if (s >= 0) scored.push_back({i, s});
  }

  // Sort by score descending.
  std::sort(scored.begin(), scored.end(),
            [](const auto& a, const auto& b) { return a.score > b.score; });

  for (const auto& s : scored) {
    filteredIndices_.push_back(s.index);
  }
}

void MenuList::moveUp() {
  rebuildFiltered();
  if (selectedIndex_ > 0) --selectedIndex_;
}

void MenuList::moveDown() {
  rebuildFiltered();
  if (selectedIndex_ < static_cast<int>(filteredIndices_.size()) - 1) ++selectedIndex_;
}

void MenuList::selectCurrent() {
  rebuildFiltered();
  if (selectedIndex_ >= 0 && selectedIndex_ < static_cast<int>(filteredIndices_.size())) {
    int realIdx = filteredIndices_[selectedIndex_];
    if (realIdx >= 0 && realIdx < static_cast<int>(items_.size())) {
      if (onSelect_) onSelect_(items_[realIdx]);
    }
  }
}

void MenuList::dismiss() {
  if (onDismiss_) onDismiss_();
}

void MenuList::setSearchQuery(const std::string& query) {
  searchQuery_ = query;
  filteredDirty_ = true;
  selectedIndex_ = 0;
}

void MenuList::appendToSearch(char ch) {
  searchQuery_ += ch;
  filteredDirty_ = true;
  selectedIndex_ = 0;
}

void MenuList::backspaceSearch() {
  if (!searchQuery_.empty()) {
    searchQuery_.pop_back();
    filteredDirty_ = true;
    selectedIndex_ = 0;
  }
}

int MenuList::itemsAreaHeight() const {
  rebuildFiltered();
  return std::min(static_cast<int>(filteredIndices_.size()), maxVisible_);
}

int MenuList::firstVisibleIndex() const {
  rebuildFiltered();
  int totalItems = static_cast<int>(filteredIndices_.size());
  int visible = std::min(totalItems, maxVisible_);
  int scrollOffset = 0;
  if (selectedIndex_ >= visible) {
    scrollOffset = selectedIndex_ - visible + 1;
  }
  scrollOffset = std::min(scrollOffset, totalItems - visible);
  if (scrollOffset < 0) scrollOffset = 0;
  return scrollOffset;
}

const MenuList::Item* MenuList::itemAtFilteredIndex(int idx) const {
  rebuildFiltered();
  if (idx < 0 || idx >= static_cast<int>(filteredIndices_.size())) return nullptr;
  int realIdx = filteredIndices_[idx];
  if (realIdx < 0 || realIdx >= static_cast<int>(items_.size())) return nullptr;
  return &items_[realIdx];
}

int MenuList::height(int /*width*/) const {
  if (!isActive()) return 0;
  rebuildFiltered();
  int itemRows = std::min(static_cast<int>(filteredIndices_.size()), maxVisible_);
  if (filteredIndices_.empty()) itemRows = 0;
  // Title (1) + separator (1) + search (1) + items + hint (1).
  return 1 + 1 + 1 + itemRows + 1;
}

std::vector<std::string> MenuList::render(int width) const {
  std::vector<std::string> lines;
  if (!isActive()) return lines;

  rebuildFiltered();

  // Title line.
  lines.push_back(theme_ansi::accent(
      ansi::bold(ansi::fitToWidth(" " + title_, width))));

  // Separator.
  lines.push_back(theme_ansi::divider(width));

  // Search line.
  std::string searchLine = theme_ansi::foreground(" /") + searchQuery_;
  if (searchQuery_.empty()) {
    searchLine += theme_ansi::dim("type to filter...");
  }
  searchLine += theme_ansi::dim("  (" +
      std::to_string(filteredIndices_.size()) + "/" +
      std::to_string(items_.size()) + ")");
  lines.push_back(ansi::fitToWidth(searchLine, width));

  if (filteredIndices_.empty()) {
    lines.push_back(theme_ansi::dim(ansi::fitToWidth("  No matches", width)));
  } else {
    // Visible window around the selected index.
    int totalItems = static_cast<int>(filteredIndices_.size());
    int visible = std::min(totalItems, maxVisible_);
    int scrollOffset = firstVisibleIndex();

    for (int i = scrollOffset; i < scrollOffset + visible && i < totalItems; ++i) {
      int realIdx = filteredIndices_[i];
      const auto& item = items_[realIdx];
      bool selected = (i == selectedIndex_);
      bool hovered = (i == hoveredIndex_);

      std::string cursor = selected ? theme_ansi::success("> ")
                                     : "  ";
      std::string label = item.label;
      if (item.marked) {
        label += " " + theme_ansi::success("\xe2\x9c\x94");
      }

      std::string row = cursor + label;
      if (!item.detail.empty()) {
        row += "  " + theme_ansi::dim(item.detail);
      }

      if (selected) {
        row = theme_ansi::selection(ansi::fitToWidth(row, width));
      } else if (hovered) {
        row = theme_ansi::altPanel(ansi::fitToWidth(row, width));
      } else {
        row = ansi::fitToWidth(row, width);
      }

      lines.push_back(row);
    }
  }

  // Hint line.
  std::string hints = theme_ansi::dim(" \xe2\x86\x91\xe2\x86\x93 navigate") +
                      theme_ansi::dim(" \xe2\x94\x82 ") +
                      theme_ansi::dim("Enter select") +
                      theme_ansi::dim(" \xe2\x94\x82 ") +
                      theme_ansi::dim("Esc cancel");
  lines.push_back(ansi::fitToWidth(hints, width));

  return lines;
}

bool MenuList::handleInput(const std::string& key) {
  if (!isActive()) return false;

  if (key == "\x1b") {
    dismiss();
    return true;
  }
  if (key == "\r" || key == "\n") {
    selectCurrent();
    dismiss();
    return true;
  }
  if (key == "\x7f" || key == "\b") {
    backspaceSearch();
    return true;
  }
  if (key == "\x1b[A") {
    moveUp();
    return true;
  }
  if (key == "\x1b[B") {
    moveDown();
    return true;
  }

  for (unsigned char ch : key) {
    if (ch >= 32 && ch < 127) {
      appendToSearch(static_cast<char>(ch));
    }
  }
  return true;
}

bool MenuList::handleMouse(const MouseEvent& event,
                           int /*screenRow*/,
                           int /*screenCol*/) {
  if (!isActive()) return false;

  if (event.type == MouseEvent::Type::Move) {
    return false;
  }

  if (event.type == MouseEvent::Type::Scroll) {
    if (event.button == MouseEvent::Button::ScrollUp) {
      moveDown();
    } else if (event.button == MouseEvent::Button::ScrollDown) {
      moveUp();
    }
    return true;
  }

  return false;
}

} // namespace firmius::tui2

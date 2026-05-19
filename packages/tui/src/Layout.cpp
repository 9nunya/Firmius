#include "Layout.hpp"

namespace firmius::tui {

void Layout::setPinnedComponents(std::vector<Component*> components) {
  pinnedComponents_ = std::move(components);
}

int Layout::pinnedHeight(int width) const {
  int total = 0;
  for (auto* comp : pinnedComponents_) {
    total += comp->height(width);
  }
  return total;
}

std::vector<std::string> Layout::renderPinned(int width) const {
  std::vector<std::string> allLines;
  for (auto* comp : pinnedComponents_) {
    auto lines = comp->render(width);
    for (auto& line : lines) {
      allLines.push_back(std::move(line));
    }
  }
  return allLines;
}

void Layout::invalidate() {
  // No cached state to invalidate in the simplified layout.
}

} // namespace firmius::tui

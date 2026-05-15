#include "Layout.hpp"

namespace firmius::tui2 {

Layout::Layout(Terminal& terminal) : terminal_(terminal) {}

void Layout::setPinnedComponents(std::vector<Component*> components) {
  pinnedComponents_ = std::move(components);
}

void Layout::renderPinned() {
  int w = width();

  // Calculate total pinned height from components.
  int totalHeight = 0;
  for (auto* comp : pinnedComponents_) {
    totalHeight += comp->height(w);
  }

  // If the pinned height changed, adjust the scroll region.
  if (totalHeight != lastTotalPinnedHeight_) {
    terminal_.setPinnedHeight(totalHeight);
    lastTotalPinnedHeight_ = totalHeight;
  }

  // Collect all rendered lines from components (top to bottom).
  std::vector<std::string> allLines;
  allLines.reserve(totalHeight);

  for (auto* comp : pinnedComponents_) {
    auto lines = comp->render(w);
    for (auto& line : lines) {
      allLines.push_back(std::move(line));
    }
  }

  // Batch the render for a single flush.
  terminal_.beginBatch();
  terminal_.renderPinned(allLines);
  terminal_.flushBatch();
}

void Layout::pushTranscriptLine(const std::string& line) {
  terminal_.beginBatch();
  terminal_.pushLine(line);
  terminal_.flushBatch();
}

void Layout::pushTranscriptLines(const std::vector<std::string>& lines) {
  if (lines.empty()) return;
  terminal_.beginBatch();
  terminal_.pushLines(lines);
  terminal_.flushBatch();
}

void Layout::invalidate() {
  // Force re-measure on next render.
  lastTotalPinnedHeight_ = -1;
}

int Layout::width() const {
  auto [w, h] = terminal_.size();
  (void)h;
  return w;
}

int Layout::height() const {
  auto [w, h] = terminal_.size();
  (void)w;
  return h;
}

} // namespace firmius::tui2

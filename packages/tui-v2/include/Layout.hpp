#pragma once

#include "Component.hpp"
#include "Terminal.hpp"

#include <vector>

namespace firmius::tui2 {

/// Dynamic layout orchestrator.
///
/// Manages the division between the scroll zone (transcript) and the
/// pinned zone (bottom components). Each frame:
///   1. Asks each pinned component for its height().
///   2. Sums → adjusts Terminal's pinned region via setPinnedHeight().
///   3. Collects render() output from each pinned component.
///   4. Calls terminal_.renderPinned() which diffs against the buffer.
///
/// Transcript content is pushed into the scroll zone independently
/// via terminal_.pushLine().
class Layout {
public:
  explicit Layout(Terminal& terminal);

  /// Set the ordered list of components that form the pinned zone
  /// (rendered bottom-to-top: last entry = bottom row of terminal).
  void setPinnedComponents(std::vector<Component*> components);

  /// Re-measure and re-render the pinned zone. Call this when state
  /// changes or on resize. Uses Terminal's batch mode internally.
  void renderPinned();

  /// Push transcript lines into the scroll zone.
  void pushTranscriptLine(const std::string& line);
  void pushTranscriptLines(const std::vector<std::string>& lines);

  /// Force a full re-layout (e.g. on terminal resize).
  void invalidate();

  /// Current terminal width.
  int width() const;

  /// Current terminal height.
  int height() const;

private:
  Terminal& terminal_;
  std::vector<Component*> pinnedComponents_;
  int lastTotalPinnedHeight_ = 0;
};

} // namespace firmius::tui2

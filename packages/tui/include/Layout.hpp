#pragma once

#include "Component.hpp"

#include <vector>

namespace firmius::tui {

/// Measures pinned components and collects their rendered output.
///
/// Pure measurement/collection helper — no terminal I/O.
/// Used by App to determine pinned zone layout and gather component lines.
class Layout {
public:
  Layout() = default;

  /// Set the ordered list of pinned components (top to bottom).
  void setPinnedComponents(std::vector<Component*> components);

  /// Total height of all pinned components at the given width.
  int pinnedHeight(int width) const;

  /// Collect rendered output from all pinned components.
  /// Returns a flat vector of styled strings (top component first).
  std::vector<std::string> renderPinned(int width) const;

  /// Invalidate cached measurements (e.g., after resize).
  void invalidate();

private:
  std::vector<Component*> pinnedComponents_;
};

} // namespace firmius::tui

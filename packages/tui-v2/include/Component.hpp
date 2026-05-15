#pragma once

#include <string>
#include <vector>

namespace firmius::tui2 {

/// Base class for all visual components.
///
/// Components NEVER touch Terminal directly. They return styled strings.
/// The layout orchestrator owns all screen writes via the Terminal's
/// diff engine.
class Component {
public:
  virtual ~Component() = default;

  /// How many terminal rows this component needs at the given width.
  virtual int height(int width) const = 0;

  /// Render into styled lines. The returned vector has exactly `height()`
  /// entries. Each string may contain ANSI escape codes.
  virtual std::vector<std::string> render(int width) const = 0;
};

} // namespace firmius::tui2

#pragma once

#include <ftxui/component/component_base.hpp>
#include <string>

namespace firmius::tui {

class TuiState; // Forward declaration

/// Base interface for all TUI modals.
/// Each modal lives in its own file and is registered by name.
/// TuiState provides the chrome (border, dimmed bg, centering) — modals
/// only return their inner content component.
class IModal {
public:
  virtual ~IModal() = default;

  /// Unique name used for registry lookup (e.g. "thread_picker")
  virtual std::string name() const = 0;

  /// Build and return the inner component for this modal.
  /// The component handles its own event logic (arrow keys, Enter, etc.)
  /// and calls state.popModal() when it wants to dismiss itself.
  virtual ftxui::Component create(TuiState &state) = 0;
};

} // namespace firmius::tui

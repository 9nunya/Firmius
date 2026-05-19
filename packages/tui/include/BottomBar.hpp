#pragma once

#include "AppState.hpp"
#include "Component.hpp"

namespace firmius::tui {

/// Bottom bar component showing contextual keybind hints.
/// Renders 1 line adapting to the current activity context.
class BottomBar : public Component {
public:
  explicit BottomBar(const AppState& state);

  int height(int width) const override;
  std::vector<std::string> render(int width) const override;

private:
  const AppState& state_;
};

} // namespace firmius::tui

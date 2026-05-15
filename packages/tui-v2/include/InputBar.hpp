#pragma once

#include "AppState.hpp"
#include "Component.hpp"

#include <string>

namespace firmius::tui2 {

/// Input bar component. Renders 1 line showing the user's input buffer
/// with a prompt indicator.
class InputBar : public Component {
public:
  explicit InputBar(const AppState& state);

  int height(int width) const override;
  std::vector<std::string> render(int width) const override;

private:
  const AppState& state_;
};

} // namespace firmius::tui2

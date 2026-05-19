#ifndef FIRMIUS_TUI_AGENTTABBAR_HPP
#define FIRMIUS_TUI_AGENTTABBAR_HPP

#include "AppState.hpp"
#include "Component.hpp"

namespace firmius::tui {

/// One-line agent tab bar showing all agents with state indicators.
/// Only rendered when multiple agents exist.
class AgentTabBar : public Component {
public:
  explicit AgentTabBar(const AppState& state);

  int height(int width) const override;
  std::vector<std::string> render(int width) const override;

private:
  const AppState& state_;
};

} // namespace firmius::tui

#endif // FIRMIUS_TUI_AGENTTABBAR_HPP

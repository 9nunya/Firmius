#pragma once

#include "AppState.hpp"
#include "Component.hpp"

namespace firmius::tui2 {

/// Multi-line status bar component.
///
/// Renders as 2 lines:
///   Line 1: Live row (agent activity phrase / token speed during streaming)
///   Line 2: NVIM-style HUD segments: [Mode] [Model] [Status] [Thread]
class StatusBar : public Component {
public:
  explicit StatusBar(const AppState& state);

  int height(int width) const override;
  std::vector<std::string> render(int width) const override;
  int liveHeight(int width) const;
  int hudHeight(int width) const;
  std::vector<std::string> renderLiveSection(int width) const;
  std::vector<std::string> renderHudSection(int width) const;

private:
  std::string renderLiveRow(int width) const;
  std::string renderHookRow(int width) const;
  std::string renderHudRow(int width) const;

  const AppState& state_;
};

} // namespace firmius::tui2

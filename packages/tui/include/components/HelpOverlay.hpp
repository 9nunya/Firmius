#ifndef FIRMIUS_TUI_HELP_OVERLAY_HPP
#define FIRMIUS_TUI_HELP_OVERLAY_HPP

#include <algorithm>
#include <ftxui/component/component.hpp>
#include <string>

namespace firmius::tui {
class TuiState;
}

namespace firmius::tui {

struct HelpOverlayLayout {
  int width = 0;
  int height = 0;
};

inline HelpOverlayLayout ComputeHelpOverlayLayout(int term_width,
                                                  int term_height) {
  HelpOverlayLayout layout;
  layout.width = std::max(80, term_width - 8);
  layout.height = std::max(22, term_height - 6);
  return layout;
}

struct HelpItem {
  std::string key;
  std::string description;
};

std::vector<HelpItem> BuildHelpItemsForSection(const std::string &section_name);
ftxui::Component HelpOverlay(TuiState &state);

} // namespace firmius::tui

#endif

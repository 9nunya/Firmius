#ifndef FIRMIUS_TUI_HELP_OVERLAY_HPP
#define FIRMIUS_TUI_HELP_OVERLAY_HPP

#include <ftxui/component/component.hpp>

namespace firmius::tui {
class TuiState;
}

namespace firmius::tui {

ftxui::Component HelpOverlay(TuiState &state);

} // namespace firmius::tui

#endif

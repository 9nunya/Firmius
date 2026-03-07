#pragma once

#include <ftxui/component/component_base.hpp>
#include <string>

namespace firmius::tui {

class TuiState;

class ThreadLockedModal {
public:
  static ftxui::Component create(TuiState &state, const std::string &threadId,
                                 int ownerPid);
};

} // namespace firmius::tui

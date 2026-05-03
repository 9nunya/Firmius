#pragma once

#include "modals/IModal.hpp"

namespace firmius::tui {

class CommandPaletteModal : public IModal {
public:
  std::string name() const override { return "command_palette"; }
  ftxui::Component create(TuiState &state) override;
};

} // namespace firmius::tui

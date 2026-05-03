#pragma once

#include "modals/IModal.hpp"

namespace firmius::tui {

class KeybindingEditorModal : public IModal {
public:
  std::string name() const override { return "keybinding_editor"; }
  ftxui::Component create(TuiState &state) override;
};

} // namespace firmius::tui

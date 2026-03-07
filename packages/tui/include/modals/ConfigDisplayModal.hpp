#pragma once

#include "modals/IModal.hpp"

namespace firmius::tui {

class ConfigDisplayModal : public IModal {
public:
  std::string name() const override { return "config_display"; }
  ftxui::Component create(TuiState &state) override;
};

} // namespace firmius::tui

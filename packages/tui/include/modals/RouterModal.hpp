#pragma once

#include "modals/IModal.hpp"

namespace firmius::tui {

class RouterModal : public IModal {
public:
  std::string name() const override { return "router"; }
  ftxui::Component create(TuiState &state) override;
};

} // namespace firmius::tui


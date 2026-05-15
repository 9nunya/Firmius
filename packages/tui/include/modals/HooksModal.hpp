#pragma once

#include "modals/IModal.hpp"

namespace firmius::tui {

class HooksModal : public IModal {
public:
  std::string name() const override { return "hooks"; }
  ftxui::Component create(TuiState &state) override;
};

} // namespace firmius::tui

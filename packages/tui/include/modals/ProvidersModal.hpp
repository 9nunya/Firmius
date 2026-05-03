#pragma once

#include "modals/IModal.hpp"

namespace firmius::tui {

class ProvidersModal : public IModal {
public:
  std::string name() const override { return "providers"; }
  ftxui::Component create(TuiState &state) override;
};

} // namespace firmius::tui

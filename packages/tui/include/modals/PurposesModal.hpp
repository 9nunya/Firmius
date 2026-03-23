#pragma once

#include "modals/IModal.hpp"

namespace firmius::tui {

class PurposesModal : public IModal {
public:
  PurposesModal();
  ~PurposesModal() override;

  std::string name() const override { return "purposes"; }
  ftxui::Component create(TuiState &state) override;
};

} // namespace firmius::tui

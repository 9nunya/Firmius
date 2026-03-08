#pragma once

#include "modals/IModal.hpp"
#include <string>

namespace firmius::tui {

class AccountsModal : public IModal {
public:
  explicit AccountsModal(std::string providerId);

  std::string name() const override { return "accounts"; }
  ftxui::Component create(TuiState &state) override;

private:
  std::string providerId_;
};

} // namespace firmius::tui

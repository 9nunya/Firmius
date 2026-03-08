#pragma once

#include "modals/IModal.hpp"
#include <string>

namespace firmius::tui {

class QuotasModal : public IModal {
public:
  explicit QuotasModal(std::string providerId);

  std::string name() const override { return "quotas"; }
  ftxui::Component create(TuiState &state) override;

private:
  std::string providerId_;
};

} // namespace firmius::tui

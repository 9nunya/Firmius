#pragma once

#include "modals/IModal.hpp"
#include "providers/BaseAPIKeyProvider.hpp"
#include <memory>
#include <string>

namespace firmius::tui {

class APIKeyWizardModal : public IModal {
public:
  APIKeyWizardModal(std::unique_ptr<firmius::provider::APIKeyWizard> wizard,
                    std::string providerName);

  std::string name() const override { return "apikey_wizard"; }
  ftxui::Component create(TuiState &state) override;

private:
  std::unique_ptr<firmius::provider::APIKeyWizard> wizard_;
  std::string providerName_;
};

} // namespace firmius::tui

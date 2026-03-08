#pragma once

#include "modals/IModal.hpp"
#include "providers/oauth/OAuthWizard.hpp"
#include <memory>
#include <string>

namespace firmius::tui {

class OAuthWizardModal : public IModal {
public:
  OAuthWizardModal(std::unique_ptr<firmius::OAuthWizard> wizard,
                   std::string providerName);

  std::string name() const override { return "oauth_wizard"; }
  ftxui::Component create(TuiState &state) override;

private:
  std::unique_ptr<firmius::OAuthWizard> wizard_;
  std::string providerName_;
};

} // namespace firmius::tui

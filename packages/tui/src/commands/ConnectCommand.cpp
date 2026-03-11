#include "commands/ConnectCommand.hpp"
#include "TUIState.hpp"
#include "modals/ConfirmationModal.hpp"
#include "modals/OAuthWizardModal.hpp"
#include "modals/APIKeyWizardModal.hpp"
#include "providers/ProviderRegistry.hpp"
#include "providers/BaseOAuthProvider.hpp"
#include "providers/BaseAPIKeyProvider.hpp"
#include <ftxui/component/component.hpp>
#include <memory>

namespace firmius::tui {

void ConnectCommand::execute(CommandCtx &ctx,
                             const std::vector<ParsedArg> &args) {
  if (args.empty())
    return;
  std::string providerName = args[0].asString();

  auto provider =
      firmius::provider::ProviderRegistry::instance().getProvider(providerName);
  if (!provider) {
    // Ideally we'd log this or show a toast, but for now we just fail silently
    return;
  }

  // Check if it's an OAuth provider
  auto oauthProvider =
      std::dynamic_pointer_cast<firmius::provider::BaseOAuthProvider>(provider);
  if (oauthProvider) {
    auto startWizard = [oauthProvider, providerName, state = ctx.state]() {
      auto wizard = oauthProvider->beginConnectionWizard();
      if (!wizard)
        return;

      auto modalObj =
          std::make_shared<OAuthWizardModal>(std::move(wizard), providerName);
      state->openModalDirect(modalObj->create(*state));
    };

    auto accounts = oauthProvider->getAccounts();
    if (!accounts.empty()) {
      auto confirmModal = std::make_shared<ConfirmationModal>(
          " Account Exists ",
          "An account already exists for " + providerName + ". Add another?",
          startWizard);
      ctx.state->openModalDirect(confirmModal->create(*ctx.state));
    } else {
      startWizard();
    }
    return;
  }

  // Check if it's an API key provider
  auto apiKeyProvider =
      std::dynamic_pointer_cast<firmius::provider::BaseAPIKeyProvider>(provider);
  if (apiKeyProvider) {
    auto startWizard = [apiKeyProvider, providerName, state = ctx.state]() {
      auto wizard = apiKeyProvider->beginConnectionWizard();
      if (!wizard)
        return;

      auto modalObj =
          std::make_shared<APIKeyWizardModal>(std::move(wizard), providerName);
      state->openModalDirect(modalObj->create(*state));
    };

    auto accounts = apiKeyProvider->getAccounts();
    if (!accounts.empty()) {
      // Show confirmation modal when adding another API key
      auto confirmModal = std::make_shared<ConfirmationModal>(
          " API Key Exists ",
          "An API key already exists for " + providerName + 
          ". Adding another key will enable automatic rotation and rate limit backoff. Continue?",
          startWizard);
      ctx.state->openModalDirect(confirmModal->create(*ctx.state));
    } else {
      startWizard();
    }
    return;
  }
}

} // namespace firmius::tui

#include "commands/ConnectCommand.hpp"
#include "TUIState.hpp"
#include "modals/ConfirmationModal.hpp"
#include "modals/OAuthWizardModal.hpp"
#include "providers/ProviderRegistry.hpp"
#include "providers/oauth/BaseOAuthProvider.hpp"
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

  auto oauthProvider =
      std::dynamic_pointer_cast<firmius::provider::BaseOAuthProvider>(provider);
  if (!oauthProvider) {
    // Provider doesn't support OAuth connection
    return;
  }

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
}

} // namespace firmius::tui

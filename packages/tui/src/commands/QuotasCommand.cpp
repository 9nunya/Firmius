#include "commands/QuotasCommand.hpp"
#include "NotificationManager.hpp"
#include "TUIState.hpp"
#include "modals/QuotasModal.hpp"
#include "providers/BaseAPIKeyProvider.hpp"
#include "providers/BaseOAuthProvider.hpp"
#include "providers/ProviderRegistry.hpp"
#include <memory>

namespace firmius::tui {

void QuotasCommand::execute(CommandCtx &ctx,
                            const std::vector<ParsedArg> &args) {
  if (args.empty() || args[0].raw_value.empty()) {
    return;
  }

  firmius::provider::ProviderRegistry::instance().hydrateProviders();
  std::string providerId = args[0].asString();
  auto provider =
      firmius::provider::ProviderRegistry::instance().getProvider(providerId);
  if (!provider) {
    NotificationManager::instance().notifyError(
        "Quotas Unavailable", "Unknown provider: " + providerId, false);
    return;
  }

  const auto oauthProvider =
      std::dynamic_pointer_cast<firmius::provider::BaseOAuthProvider>(provider);
  const auto apiKeyProvider =
      std::dynamic_pointer_cast<firmius::provider::BaseAPIKeyProvider>(
          provider);
  if (!oauthProvider &&
      !(apiKeyProvider && apiKeyProvider->supportsQuotaTracking())) {
    NotificationManager::instance().notifyWarning(
        "Quotas Unavailable",
        providerId + " does not expose quota tracking.");
    return;
  }

  auto modalObj = std::make_shared<QuotasModal>(providerId);
  ctx.state->openModalDirect(modalObj->create(*ctx.state));
}

} // namespace firmius::tui

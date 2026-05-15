#include "commands/ProvidersCommand.hpp"
#include "TUIState.hpp"
#include "providers/ProviderRegistry.hpp"

namespace firmius::tui {

void ProvidersCommand::execute(CommandCtx &ctx,
                               const std::vector<ParsedArg> &args) {
  (void)args;
  firmius::provider::ProviderRegistry::instance().hydrateProviders();
  ctx.state->refreshProviderSnapshot();
  ctx.state->refreshModelSnapshot();
  ctx.state->openModal("providers");
}

} // namespace firmius::tui

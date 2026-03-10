#include "commands/AccountsCommand.hpp"
#include "TUIState.hpp"
#include "modals/AccountsModal.hpp"
#include <memory>

namespace firmius::tui {

void AccountsCommand::execute(CommandCtx &ctx,
                              const std::vector<ParsedArg> &args) {
  if (args.empty() || args[0].raw_value.empty()) {
    return;
  }

  std::string providerId = args[0].asString();
  auto modalObj = std::make_shared<AccountsModal>(providerId);
  ctx.state->openModalDirect(modalObj->create(*ctx.state));
}

} // namespace firmius::tui

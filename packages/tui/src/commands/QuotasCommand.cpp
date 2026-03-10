#include "commands/QuotasCommand.hpp"
#include "TUIState.hpp"
#include "modals/QuotasModal.hpp"
#include <memory>

namespace firmius::tui {

void QuotasCommand::execute(CommandCtx &ctx,
                            const std::vector<ParsedArg> &args) {
  if (args.empty() || args[0].raw_value.empty()) {
    return;
  }

  std::string providerId = args[0].asString();
  auto modalObj = std::make_shared<QuotasModal>(providerId);
  ctx.state->openModalDirect(modalObj->create(*ctx.state));
}

} // namespace firmius::tui

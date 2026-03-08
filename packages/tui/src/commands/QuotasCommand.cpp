#include "commands/QuotasCommand.hpp"
#include "TUIState.hpp"
#include "modals/QuotasModal.hpp"
#include <memory>

namespace firmius::tui {

void QuotasCommand::execute(CommandCtx &ctx,
                            const std::vector<ParsedArg> &args) {
  std::string providerId = "antigravity";
  if (!args.empty() && !args[0].raw_value.empty()) {
    providerId = args[0].asString();
  }

  auto modalObj = std::make_shared<QuotasModal>(providerId);
  ctx.state->openModalDirect(modalObj->create(*ctx.state));
}

} // namespace firmius::tui

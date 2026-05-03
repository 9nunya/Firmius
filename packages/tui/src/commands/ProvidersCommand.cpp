#include "commands/ProvidersCommand.hpp"
#include "TUIState.hpp"

namespace firmius::tui {

void ProvidersCommand::execute(CommandCtx &ctx,
                               const std::vector<ParsedArg> &args) {
  (void)args;
  ctx.state->openModal("providers");
}

} // namespace firmius::tui

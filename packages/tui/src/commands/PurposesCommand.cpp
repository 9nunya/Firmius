#include "commands/PurposesCommand.hpp"
#include "TUIState.hpp"

namespace firmius::tui {

void PurposesCommand::execute(CommandCtx &ctx,
                              const std::vector<ParsedArg> &args) {
  (void)args;
  ctx.state->openModal("purposes");
}

} // namespace firmius::tui


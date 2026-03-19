#include "commands/RouterCommand.hpp"
#include "TUIState.hpp"

namespace firmius::tui {

void RouterCommand::execute(CommandCtx &ctx, const std::vector<ParsedArg> &args) {
  (void)args;
  ctx.state->openModal("router");
}

} // namespace firmius::tui


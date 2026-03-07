#include "commands/ConfigCommand.hpp"
#include "TUIState.hpp"

namespace firmius::tui {

void ConfigCommand::execute(CommandCtx &ctx,
                            const std::vector<ParsedArg> &args) {
  (void)args;
  ctx.state->openModal("config_display");
}

} // namespace firmius::tui

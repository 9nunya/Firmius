#include "commands/QuitCommand.hpp"

namespace firmius::tui {

void QuitCommand::execute(CommandCtx &ctx,
                          const std::vector<ParsedArg> &parsed_args) {
  (void)parsed_args;
  if (ctx.state) {
    ctx.state->requestQuit();
  }
}

} // namespace firmius::tui

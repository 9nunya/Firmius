#include "commands/McpCommand.hpp"
#include "TUIState.hpp"

namespace firmius::tui {

void McpCommand::execute(CommandCtx &ctx, const std::vector<ParsedArg> &args) {
  (void)args;
  ctx.state->openModal("mcp");
}

} // namespace firmius::tui

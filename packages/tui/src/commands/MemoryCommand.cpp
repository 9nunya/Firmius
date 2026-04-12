#include "commands/MemoryCommand.hpp"
#include "TUIState.hpp"

namespace firmius::tui {

void MemoryCommand::execute(CommandCtx &ctx,
                            const std::vector<ParsedArg> &args) {
  (void)args;
  ctx.state->openModal("rolling_memory_settings");
}

} // namespace firmius::tui

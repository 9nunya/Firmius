#include "commands/ThreadsCommand.hpp"
#include "TUIState.hpp"
#include <string>
#include <vector>

namespace firmius::tui {

void ThreadsCommand::execute(CommandCtx &ctx,
                             const std::vector<ParsedArg> &args) {
  (void)args;
  ctx.state->openModal("thread_picker");
}

} // namespace firmius::tui

#include "commands/ModelCommand.hpp"
#include "TUIState.hpp"

namespace firmius::tui {

void ModelCommand::execute(CommandCtx &ctx,
                           const std::vector<ParsedArg> &args) {
  (void)args;
  ctx.state->openModal("model_picker");
}

} // namespace firmius::tui

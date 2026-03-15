#include "commands/CompactCommand.hpp"
#include "harness/Harness.hpp"

namespace firmius::tui {

void CompactCommand::execute(CommandCtx &ctx,
                             const std::vector<ParsedArg> &args) {
  (void)ctx;
  (void)args;
  firmius::core::Harness::instance().compactFocusedAgent();
}

} // namespace firmius::tui

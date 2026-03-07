#include "commands/UndoCommand.hpp"
#include "harness/Harness.hpp"

namespace firmius::tui {

void UndoCommand::execute(CommandCtx &ctx, const std::vector<ParsedArg> &args) {
  (void)ctx;
  int count = 1;
  if (!args.empty()) {
    try {
      count = args.front().asInt();
    } catch (...) {
      count = 1;
    }
  }
  if (count < 1)
    count = 1;
  firmius::core::Harness::instance().undoTurns(count);
}

} // namespace firmius::tui

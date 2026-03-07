#pragma once

#include "ICommand.hpp"

namespace firmius::tui {

class UndoCommand : public ICommand {
public:
  std::string name() const override { return "undo"; }
  std::string description() const override { return "Undo the last N turns"; }
  std::vector<CommandArg> args() const override {
    return {{"count", ArgType::Number, "Number of turns to undo", true}};
  }
  void execute(CommandCtx &ctx, const std::vector<ParsedArg> &args) override;
};

} // namespace firmius::tui

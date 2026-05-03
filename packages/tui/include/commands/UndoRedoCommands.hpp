#pragma once

#include "ICommand.hpp"

namespace firmius::tui {

class UndoTurnCommand : public ICommand {
public:
  std::string name() const override { return "undo_turn"; }
  std::string description() const override {
    return "Undo the last agent turn (transcript undo with redo capture)";
  }
  std::vector<CommandArg> args() const override { return {}; }
  void execute(CommandCtx &ctx, const std::vector<ParsedArg> &args) override;
};

class RedoCommand : public ICommand {
public:
  std::string name() const override { return "redo"; }
  std::string description() const override {
    return "Redo the last transcript undo (replays undo payload)";
  }
  std::vector<CommandArg> args() const override { return {}; }
  void execute(CommandCtx &ctx, const std::vector<ParsedArg> &args) override;
};

} // namespace firmius::tui

#pragma once

#include "ICommand.hpp"

namespace firmius::tui {

class HistoryCommand : public ICommand {
public:
  std::string name() const override { return "history"; }
  std::string description() const override {
    return "Show undo/redo pointers for this session";
  }
  std::vector<CommandArg> args() const override { return {}; }
  void execute(CommandCtx &ctx, const std::vector<ParsedArg> &args) override;
};

} // namespace firmius::tui

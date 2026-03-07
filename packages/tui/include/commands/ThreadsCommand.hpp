#pragma once

#include "ICommand.hpp"

namespace firmius::tui {

class ThreadsCommand : public ICommand {
public:
  std::string name() const override { return "threads"; }
  std::string description() const override {
    return "Switch to an existing thread";
  }
  std::vector<CommandArg> args() const override { return {}; }
  void execute(CommandCtx &ctx, const std::vector<ParsedArg> &args) override;
};

} // namespace firmius::tui

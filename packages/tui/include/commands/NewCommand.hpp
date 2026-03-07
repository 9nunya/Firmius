#pragma once

#include "ICommand.hpp"

namespace firmius::tui {

class NewCommand : public ICommand {
public:
  std::string name() const override { return "new"; }
  std::string description() const override { return "Create a new thread"; }
  std::vector<CommandArg> args() const override { return {}; }
  void execute(CommandCtx &ctx, const std::vector<ParsedArg> &args) override;
};

} // namespace firmius::tui

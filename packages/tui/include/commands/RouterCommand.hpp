#pragma once

#include "ICommand.hpp"

namespace firmius::tui {

class RouterCommand : public ICommand {
public:
  std::string name() const override { return "router"; }
  std::string description() const override {
    return "Manage model routing categories";
  }
  std::vector<CommandArg> args() const override { return {}; }
  void execute(CommandCtx &ctx, const std::vector<ParsedArg> &args) override;
};

} // namespace firmius::tui


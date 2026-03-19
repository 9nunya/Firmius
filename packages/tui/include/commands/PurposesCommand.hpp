#pragma once

#include "ICommand.hpp"

namespace firmius::tui {

class PurposesCommand : public ICommand {
public:
  std::string name() const override { return "purposes"; }
  std::string description() const override {
    return "View personas and map them to model routing categories";
  }
  std::vector<CommandArg> args() const override { return {}; }
  void execute(CommandCtx &ctx, const std::vector<ParsedArg> &args) override;
};

} // namespace firmius::tui


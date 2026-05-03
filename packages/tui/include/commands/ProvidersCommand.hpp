#pragma once

#include "ICommand.hpp"

namespace firmius::tui {

class ProvidersCommand : public ICommand {
public:
  std::string name() const override { return "providers"; }
  std::string description() const override {
    return "Manage configurable provider profiles";
  }
  std::vector<CommandArg> args() const override { return {}; }
  void execute(CommandCtx &ctx, const std::vector<ParsedArg> &args) override;
};

} // namespace firmius::tui

#pragma once

#include "ICommand.hpp"

namespace firmius::tui {

class ConfigCommand : public ICommand {
public:
  std::string name() const override { return "config"; }
  std::string description() const override {
    return "Show current configuration";
  }
  std::vector<CommandArg> args() const override { return {}; }
  void execute(CommandCtx &ctx, const std::vector<ParsedArg> &args) override;
};

} // namespace firmius::tui

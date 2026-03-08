#pragma once

#include "commands/ICommand.hpp"

namespace firmius::tui {

class AccountsCommand : public ICommand {
public:
  std::string name() const override { return "accounts"; }
  std::string description() const override {
    return "Manage accounts for a provider (e.g. /accounts antigravity)";
  }
  std::vector<CommandArg> args() const override {
    return {{"provider", ArgType::String,
             "The name of the provider to manage accounts for", true}};
  }
  void execute(CommandCtx &ctx, const std::vector<ParsedArg> &args) override;
};

} // namespace firmius::tui

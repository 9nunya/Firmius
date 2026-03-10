#pragma once

#include "commands/ICommand.hpp"

namespace firmius::tui {

class AccountsCommand : public ICommand {
public:
  std::string name() const override { return "accounts"; }
  std::string description() const override {
    return "Manage accounts for an OAuth provider (usage: /accounts <provider>)";
  }
  std::vector<CommandArg> args() const override {
    return {{"provider", ArgType::OAuthProvider,
             "The name of the provider to manage accounts for", false}};
  }
  void execute(CommandCtx &ctx, const std::vector<ParsedArg> &args) override;
};

} // namespace firmius::tui

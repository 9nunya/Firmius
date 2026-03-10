#pragma once

#include "commands/ICommand.hpp"

namespace firmius::tui {

class ConnectCommand : public ICommand {
public:
  std::string name() const override { return "connect"; }
  std::string description() const override {
    return "Connect to an OAuth provider (e.g. /connect antigravity)";
  }
  std::vector<CommandArg> args() const override {
    return {{"provider", ArgType::OAuthProvider,
             "The name of the provider to connect to", false}};
  }
  void execute(CommandCtx &ctx, const std::vector<ParsedArg> &args) override;
};

} // namespace firmius::tui

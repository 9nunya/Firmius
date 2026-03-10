#pragma once

#include "commands/ICommand.hpp"

namespace firmius::tui {

class QuotasCommand : public ICommand {
public:
  std::string name() const override { return "quotas"; }
  std::string description() const override {
    return "Show quotas for an OAuth provider (usage: /quotas <provider>)";
  }
  std::vector<CommandArg> args() const override {
    return {{"provider", ArgType::OAuthProvider,
             "The name of the provider to show quotas for", false}};
  }
  void execute(CommandCtx &ctx, const std::vector<ParsedArg> &args) override;
};

} // namespace firmius::tui

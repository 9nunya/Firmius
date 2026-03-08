#pragma once

#include "commands/ICommand.hpp"

namespace firmius::tui {

class QuotasCommand : public ICommand {
public:
  std::string name() const override { return "quotas"; }
  std::string description() const override {
    return "Show quotas for a provider (e.g. /quotas antigravity)";
  }
  std::vector<CommandArg> args() const override {
    return {{"provider", ArgType::String,
             "The name of the provider to show quotas for", true}};
  }
  void execute(CommandCtx &ctx, const std::vector<ParsedArg> &args) override;
};

} // namespace firmius::tui

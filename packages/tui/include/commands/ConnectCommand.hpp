#pragma once

#include "commands/ICommand.hpp"

namespace firmius::tui {

class ConnectCommand : public ICommand {
public:
  std::string name() const override { return "connect"; }
  std::string description() const override {
    return "Connect to a provider (e.g. /connect antigravity). Supports both OAuth and API key providers.";
  }
  std::vector<CommandArg> args() const override {
    return {{"provider_id", ArgType::ProviderId,
             "The ID of the provider to connect to", false}};
  }
  void execute(CommandCtx &ctx, const std::vector<ParsedArg> &args) override;
};

} // namespace firmius::tui

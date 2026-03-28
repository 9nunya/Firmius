#pragma once

#include "commands/ICommand.hpp"

namespace firmius::tui {

class QuitCommand : public ICommand {
public:
  std::string name() const override { return "quit"; }
  std::string description() const override {
    return "Safely exit Firmius and save the current session.";
  }
  std::vector<CommandArg> args() const override { return {}; }
  void execute(CommandCtx &ctx,
               const std::vector<ParsedArg> &parsed_args) override;
};

} // namespace firmius::tui

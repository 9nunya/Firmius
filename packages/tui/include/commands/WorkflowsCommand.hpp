#pragma once

#include "ICommand.hpp"

namespace firmius::tui {

class WorkflowsCommand : public ICommand {
public:
  std::string name() const override { return "workflows"; }
  std::string description() const override {
    return "Execute or list available workflows";
  }
  std::vector<CommandArg> args() const override;
  void execute(CommandCtx &ctx,
               const std::vector<ParsedArg> &args) override;
};

} // namespace firmius::tui

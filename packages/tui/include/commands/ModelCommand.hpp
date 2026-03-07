#pragma once

#include "ICommand.hpp"

namespace firmius::tui {

class ModelCommand : public ICommand {
public:
  std::string name() const override { return "model"; }
  std::string description() const override {
    return "Switch the active LLM model";
  }
  std::vector<CommandArg> args() const override { return {}; }
  void execute(CommandCtx &ctx, const std::vector<ParsedArg> &args) override;
};

} // namespace firmius::tui

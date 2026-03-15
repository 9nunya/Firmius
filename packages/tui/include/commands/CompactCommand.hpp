#pragma once

#include "ICommand.hpp"

namespace firmius::tui {

class CompactCommand : public ICommand {
public:
  std::string name() const override { return "compact"; }
  std::string description() const override {
    return "Compact the focused agent's context";
  }
  std::vector<CommandArg> args() const override { return {}; }
  void execute(CommandCtx &ctx, const std::vector<ParsedArg> &args) override;
};

} // namespace firmius::tui

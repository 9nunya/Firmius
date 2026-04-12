#pragma once

#include "ICommand.hpp"

namespace firmius::tui {

class MemoryCommand : public ICommand {
public:
  std::string name() const override { return "memory"; }
  std::string description() const override {
    return "Configure rolling memory models and dynamic occupancy presets";
  }
  std::vector<CommandArg> args() const override { return {}; }
  void execute(CommandCtx &ctx, const std::vector<ParsedArg> &args) override;
};

} // namespace firmius::tui

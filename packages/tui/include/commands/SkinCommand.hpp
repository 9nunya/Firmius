#pragma once

#include "ICommand.hpp"

namespace firmius::tui {

class SkinCommand : public ICommand {
public:
  std::string name() const override { return "skin"; }
  std::string description() const override {
    return "Show or switch TUI skin";
  }
  std::vector<CommandArg> args() const override {
    return {{"name", ArgType::Skin, "firmius or claudex", true}};
  }
  void execute(CommandCtx &ctx, const std::vector<ParsedArg> &args) override;
};

} // namespace firmius::tui

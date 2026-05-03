#pragma once

#include "commands/ICommand.hpp"

namespace firmius::tui {

class HooksCommand : public ICommand {
public:
  std::string name() const override { return "hooks"; }
  std::string description() const override {
    return "List, reload, inspect, or replay hook workflows";
  }
  std::vector<CommandArg> args() const override {
    return {
        CommandArg{"action", ArgType::String,
                   "list, reload, state, or fire", true},
        CommandArg{"event", ArgType::String,
                   "Event for fire, for example agent_stop", true},
    };
  }
  void execute(CommandCtx &ctx, const std::vector<ParsedArg> &args) override;
};

} // namespace firmius::tui

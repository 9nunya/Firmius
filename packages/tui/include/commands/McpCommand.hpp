#pragma once

#include "ICommand.hpp"

namespace firmius::tui {

class McpCommand : public ICommand {
public:
  std::string name() const override { return "mcp"; }
  std::string description() const override {
    return "Open MCP connections modal";
  }
  std::vector<CommandArg> args() const override { return {}; }
  void execute(CommandCtx &ctx, const std::vector<ParsedArg> &args) override;
};

} // namespace firmius::tui

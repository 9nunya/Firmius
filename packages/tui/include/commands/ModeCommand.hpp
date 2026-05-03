#pragma once

#include "ICommand.hpp"

namespace firmius::tui {

/**
 * @brief `/mode [name]` — set or cycle the active mode.
 *
 *   /mode                — cycle forward (same as Ctrl+Y).
 *   /mode <name>         — switch to a specific mode. Bare names are
 *                          resolved against the active persona's sub-mode
 *                          set first, then system modes. Qualified
 *                          `persona:submode` is verbatim.
 *   /mode none|clear|off — clear the active mode (no overlay).
 *
 * Acts on the focused agent mid-thread, or on the welcome screen's
 * pre-thread `initialMode` pick.
 */
class ModeCommand : public ICommand {
public:
  std::string name() const override { return "mode"; }
  std::string description() const override {
    return "Set or cycle the agent's active mode (overlay + tool scope)";
  }
  std::vector<CommandArg> args() const override {
    return {{"name", ArgType::Mode,
             "Mode name (qualified persona:submode or bare). Optional — "
             "no argument cycles forward; 'none'/'clear' clears.",
             /*optional=*/true}};
  }
  void execute(CommandCtx &ctx, const std::vector<ParsedArg> &args) override;
};

} // namespace firmius::tui

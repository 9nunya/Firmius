#include "commands/ConfigCommand.hpp"
#include "TUIState.hpp"
#include "TUIHotkeys.hpp"

namespace firmius::tui {

void ConfigCommand::execute(CommandCtx &ctx,
                            const std::vector<ParsedArg> &args) {
  (void)args;
  ctx.state->openModal("config_display");
}

CommandBindingHints ConfigCommand::bindingHints() const {
  return {{GetHotkeyLabel(HotkeyAction::PermissionCycle),
           "Fallback permission mode cycle from config"},
          {GetHotkeyLabel(HotkeyAction::OpenCommandPalette),
           "Open the dedicated command palette"},
          {"/config → keybindings",
           "Open the interactive keybinding editor from configuration"}};
}

} // namespace firmius::tui

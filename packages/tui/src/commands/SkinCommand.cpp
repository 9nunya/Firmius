#include "commands/SkinCommand.hpp"
#include "NotificationManager.hpp"
#include "TUIState.hpp"

namespace firmius::tui {

void SkinCommand::execute(CommandCtx &ctx,
                          const std::vector<ParsedArg> &args) {
  if (!ctx.state) {
    return;
  }

  if (args.empty() || args.front().raw_value.empty()) {
    const auto current = skinKindToString(ctx.state->currentSkinKind());
    NotificationManager::instance().notifyInfo(
        "Skin",
        "Current skin: " + current + " (use /skin firmius or /skin claudex)",
        std::chrono::milliseconds(2500));
    return;
  }

  const std::string requested = args.front().asString();
  if (requested != "firmius" && requested != "claudex") {
    NotificationManager::instance().notifyWarning(
        "Unknown Skin", "Use /skin firmius or /skin claudex",
        std::chrono::milliseconds(2500));
    return;
  }

  const SkinKind kind = skinKindFromString(requested);
  ctx.state->setSkinKind(kind);
  NotificationManager::instance().notifySuccess(
      "Skin Updated", "Switched to " + skinKindToString(kind),
      std::chrono::milliseconds(1800));
}

} // namespace firmius::tui

#include "commands/NewCommand.hpp"
#include "NotificationManager.hpp"
#include "TUIState.hpp"
#include "harness/Harness.hpp"
#include <filesystem>

namespace firmius::tui {

void NewCommand::execute(CommandCtx &ctx, const std::vector<ParsedArg> &args) {
  (void)args;
  auto &h = firmius::core::Harness::instance();
  std::string cwd = std::filesystem::current_path().string();
  auto cfg = h.getConfig();
  std::string lead =
      cfg.defaultLeadPersona.empty() ? "lead" : cfg.defaultLeadPersona;
  auto *state = ctx.state;
  if (!state)
    return;
  state->clearLoadingState();
  state->setLoadingMessage("Starting new thread...");
  state->setLoadingDetail("Creating a fresh thread and preparing the lead agent.");
  state->setLoadingProgress(0.1f);
  state->runBackgroundTask([cwd, lead, state]() {
    auto &h = firmius::core::Harness::instance();
    firmius::shared::ThreadMetadata metadata;
    std::string focusedAgentId;
    bool created = false;
    try {
      const auto threadId = h.newThread({}, cwd, lead);
      created = !threadId.empty();
      if (created) {
        focusedAgentId = h.focusedAgentId();
        // Fast-path single-thread lookup; listThreads() scans the whole DB.
        metadata = h.getThreadMetadata(threadId);
        if (metadata.threadId != threadId) {
          metadata = firmius::shared::ThreadMetadata{};
        }
      }
    } catch (...) {
      created = false;
    }
    if (!created || metadata.threadId.empty()) {
      state->postAction(firmius::tui::UiThreadOpenFailed{
          "Thread", "Could not start a new thread.", true});
      return;
    }
    state->postAction(
        firmius::tui::UiThreadOpened{metadata, focusedAgentId, true});
  });
}

} // namespace firmius::tui

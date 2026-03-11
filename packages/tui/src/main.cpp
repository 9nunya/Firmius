#include "Engine.hpp"
#include "TUIState.hpp"
#include "harness/Harness.hpp"
#include "workflow/WorkflowLoader.hpp"
#include <ftxui/component/component.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/screen.hpp>
#include <string>

#include "commands/AccountsCommand.hpp"
#include "commands/CommandManager.hpp"
#include "commands/ConfigCommand.hpp"
#include "commands/ConnectCommand.hpp"
#include "commands/ModelCommand.hpp"
#include "commands/NewCommand.hpp"
#include "commands/QuotasCommand.hpp"
#include "commands/ThreadsCommand.hpp"
#include "commands/UndoCommand.hpp"
#include "commands/WorkflowsCommand.hpp"
#include "modals/ConfigDisplayModal.hpp"
#include "modals/ModalRegistry.hpp"
#include "modals/ModelPickerModal.hpp"
#include "modals/ThreadLockedModal.hpp"
#include "modals/ThreadPickerModal.hpp"

using namespace ftxui;

int main(int argc, char **argv) {
  bool continue_last = false;
  bool debugging_mode = false;
  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    if (arg == "-c") {
      continue_last = true;
    } else if (arg == "--i-am-debugging") {
      debugging_mode = true;
    }
  }

  // Register Commands
  firmius::tui::CommandManager::instance().registerCommand(
      std::make_shared<firmius::tui::NewCommand>());
  firmius::tui::CommandManager::instance().registerCommand(
      std::make_shared<firmius::tui::ThreadsCommand>());
  firmius::tui::CommandManager::instance().registerCommand(
      std::make_shared<firmius::tui::ModelCommand>());
  firmius::tui::CommandManager::instance().registerCommand(
      std::make_shared<firmius::tui::UndoCommand>());
  firmius::tui::CommandManager::instance().registerCommand(
      std::make_shared<firmius::tui::ConfigCommand>());
  firmius::tui::CommandManager::instance().registerCommand(
      std::make_shared<firmius::tui::ConnectCommand>());
  firmius::tui::CommandManager::instance().registerCommand(
      std::make_shared<firmius::tui::QuotasCommand>());
  firmius::tui::CommandManager::instance().registerCommand(
      std::make_shared<firmius::tui::AccountsCommand>());
  // Note: /workflows command removed - workflows are now registered as individual commands below

  // Register Modals
  firmius::tui::ModalRegistry::instance().registerModal(
      std::make_shared<firmius::tui::ThreadPickerModal>());
  firmius::tui::ModalRegistry::instance().registerModal(
      std::make_shared<firmius::tui::ModelPickerModal>());
  firmius::tui::ModalRegistry::instance().registerModal(
      std::make_shared<firmius::tui::ConfigDisplayModal>());

  auto &h = firmius::core::Harness::instance();
  h.init();

  // Initialize workflow loader after harness (loads from ~/.firmius/workflows/)
  firmius::core::WorkflowLoader::instance().init();

  // Register each workflow as its own command (e.g., /parallel_exploration)
  firmius::tui::registerWorkflowCommands();

  auto &state = firmius::tui::TuiState::instance();

  bool thread_loaded = false;
  if (debugging_mode) {
    firmius::shared::HostCreationOptions opts;
    opts.type = firmius::shared::HostType::Docker;
    opts.containerName = "firmius-debugging";
    opts.connectToExisting = true;
    opts.deleteOnExit = false;

    std::string cwd = "/work";
    if (!h.newThread(opts, cwd, "firmius").empty()) {
      thread_loaded = true;
    }
  } else if (continue_last) {
    if (h.resumeLast()) {
      thread_loaded = true;
    }
  }

  if (thread_loaded) {
    auto current_id = h.currentThreadId();
    firmius::shared::ThreadMetadata current_metadata;
    for (const auto &m : h.listThreads()) {
      if (m.threadId == current_id) {
        current_metadata = m;
        break;
      }
    }
    state.init(h, current_metadata, h.focusedAgentId());
    state.setViewMode(firmius::tui::TuiState::ViewMode::Chat);
  } else {
    firmius::shared::ThreadMetadata dummy_thread;
    state.init(h, dummy_thread, "");
    state.setViewMode(firmius::tui::TuiState::ViewMode::Welcome);
  }

  auto screen = ftxui::ScreenInteractive::Fullscreen();
  screen.TrackMouse(true);
  state.attachScreen(&screen);

  auto renderer = state.root();
  renderer = CatchEvent(renderer, [&](Event event) {
    if (event.is_character() && event.character() == std::string(1, '\x03')) {
      screen.ExitLoopClosure()();
      return true;
    }
    return false;
  });

  screen.Loop(renderer);
  state.shutdown();
  h.shutdown();
  firmius::core::Engine::instance().shutdown();

  return 0;
}

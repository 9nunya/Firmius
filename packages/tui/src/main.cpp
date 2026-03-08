#include "TUIState.hpp"
#include "Engine.hpp"
#include "harness/Harness.hpp"
#include <ftxui/component/component.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/screen.hpp>
#include <string>
#include <filesystem>

#include "commands/CommandManager.hpp"
#include "commands/ConfigCommand.hpp"
#include "commands/ModelCommand.hpp"
#include "commands/NewCommand.hpp"
#include "commands/ThreadsCommand.hpp"
#include "commands/UndoCommand.hpp"
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

  // Register Modals
  firmius::tui::ModalRegistry::instance().registerModal(
      std::make_shared<firmius::tui::ThreadPickerModal>());
  firmius::tui::ModalRegistry::instance().registerModal(
      std::make_shared<firmius::tui::ModelPickerModal>());
  firmius::tui::ModalRegistry::instance().registerModal(
      std::make_shared<firmius::tui::ConfigDisplayModal>());

  auto &h = firmius::core::Harness::instance();
  h.init();

  auto &state = firmius::tui::TuiState::instance();

  // We attach dummy thread/focused initially
  firmius::shared::ThreadMetadata dummy_thread;
  state.init(h, dummy_thread, "");

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

  if (!thread_loaded) {
    state.setViewMode(firmius::tui::TuiState::ViewMode::Welcome);
  } else {
    state.setViewMode(firmius::tui::TuiState::ViewMode::Chat);
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

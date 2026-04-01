#include "TuiRunner.hpp"
#include "TUIState.hpp"
#include "harness/Harness.hpp"
#include "workflow/WorkflowLoader.hpp"
#include <atomic>
#include <chrono>
#include <csignal>
#include <ftxui/component/component.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/screen.hpp>
#include <iostream>
#include <string>
#include <thread>

#include "commands/AccountsCommand.hpp"
#include "commands/BenchmarksCommand.hpp"
#include "commands/CommandManager.hpp"
#include "commands/CompactCommand.hpp"
#include "commands/ConfigCommand.hpp"
#include "commands/ConnectCommand.hpp"
#include "commands/ModelCommand.hpp"
#include "commands/NewCommand.hpp"
#include "commands/PurposesCommand.hpp"
#include "commands/QuitCommand.hpp"
#include "commands/QuotasCommand.hpp"
#include "commands/RouterCommand.hpp"
#include "commands/ThreadsCommand.hpp"
#include "commands/UndoCommand.hpp"
#include "commands/WorkflowsCommand.hpp"
#include "modals/ConfigDisplayModal.hpp"
#include "modals/ModalRegistry.hpp"
#include "modals/ModelPickerModal.hpp"
#include "modals/PurposesModal.hpp"
#include "modals/RouterModal.hpp"
#include "modals/ThreadLockedModal.hpp"
#include "modals/ThreadPickerModal.hpp"

namespace {

extern "C" void HandleSigint(int) {
  g_pending_sigint.fetch_add(1, std::memory_order_relaxed);
}

void InstallSigintHandler() {
  struct sigaction action{};
  action.sa_handler = HandleSigint;
  sigemptyset(&action.sa_mask);
  action.sa_flags = 0;
  sigaction(SIGINT, &action, nullptr);
}

} // namespace

namespace firmius::tui {

void runTui(bool debugging_mode, bool continue_last) {
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
      std::make_shared<firmius::tui::CompactCommand>());
  firmius::tui::CommandManager::instance().registerCommand(
      std::make_shared<firmius::tui::ConnectCommand>());
  firmius::tui::CommandManager::instance().registerCommand(
      std::make_shared<firmius::tui::QuotasCommand>());
  firmius::tui::CommandManager::instance().registerCommand(
      std::make_shared<firmius::tui::AccountsCommand>());
  firmius::tui::CommandManager::instance().registerCommand(
      std::make_shared<firmius::tui::QuitCommand>());
  firmius::tui::CommandManager::instance().registerCommand(
      std::make_shared<firmius::tui::RouterCommand>());
  firmius::tui::CommandManager::instance().registerCommand(
      std::make_shared<firmius::tui::PurposesCommand>());
  firmius::tui::CommandManager::instance().registerCommand(
      std::make_shared<firmius::tui::BenchmarksCommand>());
  // Note: /workflows command removed - workflows are now registered as
  // individual commands below

  // Register Modals
  firmius::tui::ModalRegistry::instance().registerModal(
      std::make_shared<firmius::tui::ThreadPickerModal>());
  firmius::tui::ModalRegistry::instance().registerModal(
      std::make_shared<firmius::tui::ModelPickerModal>());
  firmius::tui::ModalRegistry::instance().registerModal(
      std::make_shared<firmius::tui::ConfigDisplayModal>());
  firmius::tui::ModalRegistry::instance().registerModal(
      std::make_shared<firmius::tui::RouterModal>());
  firmius::tui::ModalRegistry::instance().registerModal(
      std::make_shared<firmius::tui::PurposesModal>());

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
    auto cfg = h.getConfig();
    std::string lead =
        cfg.defaultLeadPersona.empty() ? "lead" : cfg.defaultLeadPersona;
    if (!h.newThread(opts, cwd, lead).empty()) {
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
  screen.ForceHandleCtrlC(false);
  state.attachScreen(&screen);
  InstallSigintHandler();

  // Enable bracketed paste mode so terminal sends \x1b[200~ and \x1b[201~
  // sequences around pasted content, allowing us to detect multi-line pastes
  std::cout << "\x1b[?2004h" << std::flush;

  auto renderer = state.root();
  std::atomic<bool> sigint_bridge_running{true};
  std::jthread sigint_bridge([&](std::stop_token st) {
    int last_seen = 0;
    while (!st.stop_requested() && sigint_bridge_running.load()) {
      InstallSigintHandler();
      const int current = g_pending_sigint.load(std::memory_order_relaxed);
      if (current != last_seen) {
        last_seen = current;
        screen.PostEvent(ftxui::Event::Custom);
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
  });
  int handled_sigints = 0;
  renderer = CatchEvent(renderer, [&](ftxui::Event event) {
    const int pending_sigints =
        g_pending_sigint.load(std::memory_order_relaxed);
    if (pending_sigints > handled_sigints) {
      handled_sigints = pending_sigints;
      return state.handleCtrlC();
    }
    if (event == ftxui::Event::CtrlC ||
        (event.is_character() && event.character() == std::string(1, '\x03'))) {
      return state.handleCtrlC();
    }
    return false;
  });

  screen.Loop(renderer);
  sigint_bridge_running = false;
  sigint_bridge.request_stop();
  std::signal(SIGINT, SIG_DFL);
  std::cout << "\x1b[?2004l" << std::flush;
  const std::string exit_summary = state.exitSummaryText();
  state.shutdown();
  h.shutdown();
  std::cout << exit_summary << std::flush;
}

} // namespace firmius::tui

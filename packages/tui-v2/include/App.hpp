#pragma once

#include "ActionDispatcher.hpp"
#include "AppState.hpp"
#include "BottomBar.hpp"
#include "CommandManager.hpp"
#include "DaemonSession.hpp"
#include "EventRouter.hpp"
#include "InputBar.hpp"
#include "KeybindRegistry.hpp"
#include "Layout.hpp"
#include "MenuList.hpp"
#include "StatusBar.hpp"
#include "Terminal.hpp"
#include "TranscriptRenderer.hpp"

#include <atomic>
#include <string>

namespace firmius::tui2 {

/// CLI arguments for launching the TUI.
struct AppOptions {
  std::string threadId;       ///< Resume specific thread.
  std::string persona = "lead";
  std::string mode;           ///< Initial mode (e.g., "forge").
  std::string cwd;            ///< Working directory.
  bool continueSession = false;
};

/// Main application orchestrator.
class App {
public:
  explicit App(AppOptions options);
  ~App();

  /// Run the main loop. Blocks until exit.
  int run();

private:
  // Initialization.
  void registerCommands();
  void setupKeybinds();
  void connectAndSetup();

  // Main loop phases.
  void handleInput(const std::string& key);
  void renderFrame();
  void onResize();

  // Command handlers.
  void openModelsMenu();
  void openResumeMenu();
  void dismissMenu();

  AppOptions options_;
  Terminal terminal_;
  Layout layout_;
  AppState state_;
  DaemonSession session_;
  EventRouter eventRouter_;
  ActionDispatcher dispatcher_;
  CommandManager commands_;
  KeybindRegistry keybinds_;

  // Components.
  TranscriptRenderer transcriptRenderer_;
  StatusBar statusBar_;
  InputBar inputBar_;
  BottomBar bottomBar_;
  MenuList menu_;

  std::atomic<bool> running_{false};
  std::string lastStreamingText_;

  // Commands defined in App.cpp need access to private members.
  friend class QuitCmd;
  friend class NewCmd;
  friend class ModelsCmd;
  friend class ResumeCmd;
};

} // namespace firmius::tui2

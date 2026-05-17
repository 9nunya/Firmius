#pragma once

#include "ActionDispatcher.hpp"
#include "AgentTabBar.hpp"
#include "AppState.hpp"
#include "BottomBar.hpp"
#include "Cell.hpp"
#include "CommandManager.hpp"
#include "DaemonSession.hpp"
#include "EventRouter.hpp"
#include "AccountsOverlay.hpp"
#include "InfoOverlay.hpp"
#include "InputBar.hpp"
#include "KeybindRegistry.hpp"
#include "Layout.hpp"
#include "MenuList.hpp"
#include "Overlay.hpp"
#include "StatusBar.hpp"
#include "Terminal.hpp"
#include <atomic>
#include <chrono>
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
  void handleMouse(const MouseEvent& event);
  void reconcileRuntimeState();
  void renderFrame();
  void onResize();

  // Full-screen rendering pipeline.
  void composeFrame(CellGrid& target, int w, int h);
  void renderTranscriptZone(CellGrid& target, int w, int transcriptH);
  void renderPinnedZone(CellGrid& target, int w, int h, int pinnedH);
  void diffAndEmit(const CellGrid& target, int w, int h);

  // Scrollback management.
  void syncScrollback();

  // Command handlers.
  void openModelsMenu();
  void openResumeMenu();
  void openAccountsOverlay(const std::string& providerId);
  void applyTheme(const std::string& name);
  void dismissMenu();
  void dismissOverlay();
  void submitInputBuffer();
  void applyModelSelection(const std::string& providerId,
                           const std::string& modelId,
                           uint32_t contextWindowTokens);

  // Agent focus management.
  void switchToAgentTranscript(const std::string& agentId);

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
  StatusBar statusBar_;
  AgentTabBar agentTabBar_;
  InputBar inputBar_;
  BottomBar bottomBar_;
  MenuList menuList_;
  InfoOverlay infoOverlay_;
  AccountsOverlay accountsOverlay_;
  Overlay* activeOverlay_ = nullptr;

  // Focus tracking.
  std::string lastFocusedAgentId_;

  // Full-screen rendering state.
  std::atomic<bool> running_{false};
  CellGrid prevFrame_;         ///< Previous frame's cells for diffing.
  int prevW_ = 0, prevH_ = 0; ///< Dimensions of prevFrame_.

  // Scrollback sync tracking.
  size_t lastSyncedItemCount_ = 0;  ///< How many items have been synced to scrollback.
  std::vector<int> lastSyncedRowCounts_; ///< Row count per item at last sync.

  std::chrono::steady_clock::time_point lastRuntimeReconcile_{};

  // Autocomplete state.
  struct AutocompleteState {
    bool active = false;
    std::vector<AutocompleteMatch> matches;
    int selectedIndex = 0;
    std::string prefix;  // The "/" prefix being typed.
  };
  AutocompleteState autocomplete_;

  void updateAutocomplete();
  void dismissAutocomplete();
  void autocompleteMoveUp();
  void autocompleteMoveDown();
  void autocompleteAccept();

  // Commands defined in App.cpp need access to private members.
  friend class QuitCmd;
  friend class NewCmd;
  friend class ModelsCmd;
  friend class ResumeCmd;
  friend class AccountsCmd;
  friend class ThemeCmd;
  friend class AccountsOverlay;
};

} // namespace firmius::tui2

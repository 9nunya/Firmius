#ifndef FIRMIUS_TUI_APP_HPP
#define FIRMIUS_TUI_APP_HPP

#include "ActionDispatcher.hpp"
#include "AgentTabBar.hpp"
#include "AppState.hpp"
#include "BottomBar.hpp"
#include "Cell.hpp"
#include "CommandManager.hpp"
#include "DaemonSession.hpp"
#include "EventRouter.hpp"
#include "AccountsOverlay.hpp"
#include "ConnectOverlay.hpp"
#include "InfoOverlay.hpp"
#include "NotificationCenter.hpp"
#include "NotificationStrip.hpp"
#include "RewindOverlay.hpp"
#include "RedoOverlay.hpp"
#include "RouterOverlay.hpp"
#include "PurposesOverlay.hpp"
#include "PermissionPromptOverlay.hpp"
#include "PermissionsOverlay.hpp"
#include "InputBar.hpp"
#include "KeybindRegistry.hpp"
#include "Layout.hpp"
#include "MenuList.hpp"
#include "Overlay.hpp"
#include "StatusBar.hpp"
#include "Terminal.hpp"
#include <atomic>
#include <chrono>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace firmius::tui {

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
  void openConnectOverlay(const std::string& providerId);
  /// Internal: actually start the wizard after any "add another?" confirm.
  void beginConnectFlow(const std::string& providerId, bool addAdditional);
  /// Forward a ConnectProgress event from EventRouter to the active overlay.
  void onConnectProgress(const firmius::daemon::ConnectProgressSnapshot& snap);
  void openRewindOverlay();
  /// Triggered when the user highlights a turn — fires rewind.preview RPC.
  void onRewindPreviewRequest(const std::string& targetTurnId);
  /// Triggered when the user picks a mode — fires rewind.execute RPC.
  void onRewindExecuteRequest(const std::string& targetTurnId,
                               firmius::daemon::RewindMode mode);
  /// Forward a RewindApplied event from EventRouter into overlay + state.
  void onRewindApplied(const firmius::daemon::RewindAppliedSnapshot& snap);

  /// Open the /redo overlay. Pre-fetches the recent transcript-undo
  /// actions list synchronously and seeds the picker.
  void openRedoOverlay();
  /// Triggered when the user picks a mode in the redo overlay.
  void onRedoExecuteRequest(const std::string& undoActionId,
                             firmius::daemon::RedoMode mode);

  void openRouterOverlay();
  void openPurposesOverlay();
  /// Open the permission prompt overlay for the front of the queue.
  void openPermissionPromptOverlay();
  /// Open the /permissions overlay (rule viewer/editor).
  void openPermissionsOverlay();

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
  // Notifications. The center owns state; the strip is the renderer.
  // Public so command/overlay code can push toasts without going
  // through every wrapper. (Pushes are mutex-protected internally.)
  NotificationCenter notifications_;
  NotificationStrip notificationStrip_;
  MenuList menuList_;
  InfoOverlay infoOverlay_;
  AccountsOverlay accountsOverlay_;
  ConnectOverlay connectOverlay_;
  RewindOverlay rewindOverlay_;
  RedoOverlay redoOverlay_;
  RouterOverlay routerOverlay_;
  PurposesOverlay purposesOverlay_;
  PermissionPromptOverlay permissionOverlay_;
  PermissionsOverlay permissionsOverlay_;
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
  //
  // The dropdown is "scrollable": we keep a logical list of `matches` and a
  // `scrollOffset` that controls which slice of `matches` is visible. The
  // visible window is `kVisibleRows` high. Arrow keys move `selectedIndex`
  // through the full list and tug `scrollOffset` along when the cursor would
  // leave the window.
  //
  // Two modes:
  //   - Command name (`mode == CommandName`): typing right after `/`. Matches
  //     are command names from CommandManager.
  //   - Arg value (`mode == ArgValue`): user has typed `/cmd ` and is typing
  //     the Nth argument. Matches come from the daemon based on the arg's
  //     type (ProviderId → provider list, ThreadId → thread list, etc.).
  //
  // `inputSnapshot` is the input buffer when matches were last computed; we
  // use it to detect when the input has changed in ways that need a refetch.
  struct AutocompleteState {
    enum class Mode { CommandName, ArgValue };
    bool active = false;
    Mode mode = Mode::CommandName;
    std::vector<AutocompleteMatch> matches;
    int selectedIndex = 0;
    int scrollOffset = 0;
    /// Snapshot of the input buffer at the time matches were generated. Used
    /// by accept() to figure out exactly which slice of the buffer to replace.
    std::string inputSnapshot;
    /// Active command (set when mode == ArgValue).
    std::string activeCommandName;
    /// Index of the arg currently being typed (set when mode == ArgValue).
    int currentArgIndex = -1;
    /// Filter the user has typed for the current arg (everything after the
    /// last space, before the cursor).
    std::string argFilter;
  };
  AutocompleteState autocomplete_;
  static constexpr int kAutocompleteVisibleRows = 5;

  // ── Bracketed paste accumulator ──────────────────────────────────────
  // The terminal wraps pasted content in \x1b[200~ ... \x1b[201~. We
  // accumulate the bytes between those markers, then commit to the input
  // buffer at the end. If the pasted content looks like base64 image data
  // (very long single line), we treat it as an image and store it on a
  // pending list; otherwise it's inserted as text.
  bool inBracketedPaste_ = false;
  std::string pasteBuffer_;
  /// Pending images collected from paste. Drained when the user submits.
  std::vector<firmius::shared::ImageContent> pendingPastedImages_;
  void commitPasteBuffer();

  // ── Deferred actions ────────────────────────────────────────────────
  // Daemon event listeners run on the JsonRpcTransport reader thread.
  // From there we MUST NOT issue another blocking RPC — that would
  // deadlock the same reader (it's the only thread that can deliver
  // the response to our nested RPC) and time out a few seconds later.
  //
  // Anything that needs to make a follow-up RPC (e.g. transcript reload
  // after RewindApplied) gets queued here and drained from the main
  // loop's tick.
  std::mutex deferredMutex_;
  std::vector<std::function<void()>> deferredActions_;
  void postDeferred(std::function<void()> action);
  void drainDeferredActions();

  void updateAutocomplete();
  void dismissAutocomplete();
  void autocompleteMoveUp();
  void autocompleteMoveDown();
  void autocompleteAccept();
  /// Refresh matches without resetting selection/scroll. Called from
  /// updateAutocomplete and after arg suggestions are fetched async.
  void refreshAutocompleteMatches();
  /// Build a list of suggestions for a given arg type. Returns empty when
  /// the type has no static suggestion set (e.g. String, Number).
  std::vector<AutocompleteMatch>
  fetchArgSuggestions(ArgType type, const std::string& filter);

  // Commands defined in App.cpp need access to private members.
  friend class QuitCmd;
  friend class NewCmd;
  friend class ModelsCmd;
  friend class ResumeCmd;
  friend class AccountsCmd;
  friend class ThemeCmd;
  friend class ConnectCmd;
  friend class UndoCmd;
  friend class RedoCmd;
  friend class RouterCmd;
  friend class PurposesCmd;
  friend class PermissionsCmd;
  friend class AccountsOverlay;
};

} // namespace firmius::tui

#endif // FIRMIUS_TUI_APP_HPP

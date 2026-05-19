#include "App.hpp"
#include "AnsiParser.hpp"
#include "Clipboard.hpp"
#include "Terminal.hpp"
#include "ThemeAnsi.hpp"
#include "ThemeManager.hpp"
#include "WorkflowCommand.hpp"
#include "items/SimpleItems.hpp"
#include "items/StreamingItems.hpp"
#include "items/ToolCallItem.hpp"
#include "tools/PresenterInit.hpp"
#include "workflow/WorkflowLoader.hpp"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <sstream>
#include <thread>
#include <unordered_map>

namespace firmius::tui {

namespace {

std::string humanizeTokenWindow(uint32_t value) {
  if (value >= 1000000) return std::to_string(value / 1000000) + "M";
  if (value >= 1000) return std::to_string(value / 1000) + "k";
  return std::to_string(value);
}

std::string formatClockTime(std::time_t when) {
  std::tm local{};
  localtime_r(&when, &local);
  char buffer[32];
  std::strftime(buffer, sizeof(buffer), "%I:%M %p", &local);
  std::string out = buffer;
  if (!out.empty() && out[0] == '0') {
    out.erase(out.begin());
  }
  return out;
}

std::string formatRelativeThreadTime(uint64_t epochMs) {
  if (epochMs == 0) {
    return "unknown activity";
  }

  const auto then = std::chrono::system_clock::time_point{std::chrono::milliseconds(epochMs)};
  const auto now = std::chrono::system_clock::now();
  const auto thenTime = std::chrono::system_clock::to_time_t(then);
  const auto nowTime = std::chrono::system_clock::to_time_t(now);

  std::tm thenLocal{};
  std::tm nowLocal{};
  localtime_r(&thenTime, &thenLocal);
  localtime_r(&nowTime, &nowLocal);

  auto startOfDay = [](std::tm tm) {
    tm.tm_hour = 0;
    tm.tm_min = 0;
    tm.tm_sec = 0;
    return std::mktime(&tm);
  };

  const auto dayDiffSeconds = std::difftime(startOfDay(nowLocal), startOfDay(thenLocal));
  const int dayDiff = static_cast<int>(dayDiffSeconds / (60 * 60 * 24));
  const std::string clock = formatClockTime(thenTime);

  if (dayDiff == 0) {
    return "today at " + clock;
  }
  if (dayDiff == 1) {
    return "yesterday at " + clock;
  }

  char dateBuffer[40];
  std::strftime(dateBuffer, sizeof(dateBuffer), "%b %e", &thenLocal);
  std::string date = dateBuffer;
  while (!date.empty() && date[date.size() - 1] == ' ') {
    date.pop_back();
  }
  return date + " at " + clock;
}

std::string threadCountsDetail(std::size_t agentCount, std::size_t artifactCount) {
  std::ostringstream detail;
  detail << agentCount << " agent" << (agentCount == 1 ? "" : "s");
  if (artifactCount > 0) {
    detail << " · " << artifactCount << " artifact"
           << (artifactCount == 1 ? "" : "s");
  }
  return detail.str();
}

std::string lockDetail(const firmius::daemon::ThreadOverview& overview) {
  if (overview.lockOwnerPid <= 0) {
    return "";
  }
  std::string detail = "locked by pid " + std::to_string(overview.lockOwnerPid);
  if (!overview.lockOwnerUiKind.empty()) {
    detail += " (" + overview.lockOwnerUiKind + ")";
  }
  return detail;
}

bool hasVisibleOverlay(const Cell& cell) {
  return cell.ch != ' ' || cell.fg.type != CellColor::Type::Default ||
         cell.bg.type != CellColor::Type::Default || cell.style.bold ||
         cell.style.dim || cell.style.italic || cell.style.underline ||
         cell.style.strikethrough || cell.style.invert;
}

void applyFallbackBackground(Cell& cell, const ThemeSpec& theme) {
  if (cell.ch != ' ' && cell.bg.type == CellColor::Type::Default) {
    cell.bg.type = CellColor::Type::RGB;
    cell.bg.r = static_cast<uint8_t>(theme.chat.bg.r);
    cell.bg.g = static_cast<uint8_t>(theme.chat.bg.g);
    cell.bg.b = static_cast<uint8_t>(theme.chat.bg.b);
  }
}

void blendCell(Cell& base, const Cell& overlay) {
  if (!hasVisibleOverlay(overlay)) {
    return;
  }
  if (overlay.ch != ' ') {
    base.ch = overlay.ch;
  }
  if (overlay.fg.type != CellColor::Type::Default) {
    base.fg = overlay.fg;
  }
  if (overlay.bg.type != CellColor::Type::Default) {
    base.bg = overlay.bg;
  }
  base.style = overlay.style;
}

}

// ── Inline command classes (defined before registerCommands) ──

class QuitCmd : public ICommand {
public:
  explicit QuitCmd(App& app) : app_(app) {}
  std::string name() const override { return "quit"; }
  std::string description() const override { return "Exit the terminal."; }
  void execute(const std::vector<ParsedArg>&) override { app_.running_ = false; }
private:
  App& app_;
};

class NewCmd : public ICommand {
public:
  explicit NewCmd(App& app) : app_(app) {}
  std::string name() const override { return "new"; }
  std::string description() const override { return "Create a new thread."; }
  void execute(const std::vector<ParsedArg>&) override {
    app_.state_.clearItems();
    app_.dispatcher_.createThread(app_.options_.persona, app_.options_.mode);
  }
private:
  App& app_;
};

class ModelsCmd : public ICommand {
public:
  explicit ModelsCmd(App& app) : app_(app) {}
  std::string name() const override { return "models"; }
  std::string description() const override { return "Switch LLM model."; }
  void execute(const std::vector<ParsedArg>&) override { app_.openModelsMenu(); }
private:
  App& app_;
};

class ResumeCmd : public ICommand {
public:
  explicit ResumeCmd(App& app) : app_(app) {}
  std::string name() const override { return "resume"; }
  std::string description() const override { return "Resume a previous session."; }
  void execute(const std::vector<ParsedArg>&) override { app_.openResumeMenu(); }
private:
  App& app_;
};

class AccountsCmd : public ICommand {
public:
  explicit AccountsCmd(App& app) : app_(app) {}
  std::string name() const override { return "accounts"; }
  std::string description() const override {
    return "Show accounts and quotas for a provider (usage: /accounts <provider>)";
  }
  std::vector<CommandArg> args() const override {
    return {{"provider", ArgType::ProviderId,
             "The provider to show accounts for", false}};
  }
  void execute(const std::vector<ParsedArg>& args) override {
    if (args.empty() || args[0].rawValue.empty()) return;
    app_.openAccountsOverlay(args[0].asString());
  }
private:
  App& app_;
};

class ConnectCmd : public ICommand {
public:
  explicit ConnectCmd(App& app) : app_(app) {}
  std::string name() const override { return "connect"; }
  std::string description() const override {
    return "Connect to a provider (usage: /connect <provider>). Drives the "
           "OAuth or API-key wizard exposed by the daemon.";
  }
  std::vector<CommandArg> args() const override {
    return {{"provider", ArgType::ProviderId,
             "The provider to connect to (e.g. anthropic, antigravity)", false}};
  }
  void execute(const std::vector<ParsedArg>& args) override {
    if (args.empty() || args[0].rawValue.empty()) return;
    app_.openConnectOverlay(args[0].asString());
  }
private:
  App& app_;
};

class UndoCmd : public ICommand {
public:
  explicit UndoCmd(App& app) : app_(app) {}
  std::string name() const override { return "undo"; }
  std::string description() const override {
    return "Rewind to a previous user message — restore code, conversation, or both.";
  }
  std::vector<CommandArg> args() const override { return {}; }
  void execute(const std::vector<ParsedArg>& /*args*/) override {
    app_.openRewindOverlay();
  }
private:
  App& app_;
};

class RedoCmd : public ICommand {
public:
  explicit RedoCmd(App& app) : app_(app) {}
  std::string name() const override { return "redo"; }
  std::string description() const override {
    return "Redo a previous /undo — replay the discarded turns and/or restore the code edits.";
  }
  std::vector<CommandArg> args() const override { return {}; }
  void execute(const std::vector<ParsedArg>& /*args*/) override {
    app_.openRedoOverlay();
  }
private:
  App& app_;
};

class RouterCmd : public ICommand {
public:
  explicit RouterCmd(App& app) : app_(app) {}
  std::string name() const override { return "router"; }
  std::string description() const override {
    return "Manage model routing categories and fallback order.";
  }
  std::vector<CommandArg> args() const override { return {}; }
  void execute(const std::vector<ParsedArg>&) override {
    app_.openRouterOverlay();
  }
private:
  App& app_;
};

class PurposesCmd : public ICommand {
public:
  explicit PurposesCmd(App& app) : app_(app) {}
  std::string name() const override { return "purposes"; }
  std::string description() const override {
    return "Map personas to model routing categories.";
  }
  std::vector<CommandArg> args() const override { return {}; }
  void execute(const std::vector<ParsedArg>&) override {
    app_.openPurposesOverlay();
  }
private:
  App& app_;
};

class PermissionsCmd : public ICommand {
public:
  explicit PermissionsCmd(App& app) : app_(app) {}
  std::string name() const override { return "permissions"; }
  std::string description() const override {
    return "View and manage permission policy rules.";
  }
  std::vector<CommandArg> args() const override { return {}; }
  void execute(const std::vector<ParsedArg>&) override {
    app_.openPermissionsOverlay();
  }
private:
  App& app_;
};

class ThemeCmd : public ICommand {
public:
  explicit ThemeCmd(App& app) : app_(app) {}
  std::string name() const override { return "theme"; }
  std::string description() const override { return "List or switch TUI theme."; }
  std::vector<CommandArg> args() const override {
    return {{"name", ArgType::String, "Theme name", true}};
  }
  bool takesRawRemainder() const override { return true; }
  void execute(const std::vector<ParsedArg>& args) override {
    if (args.empty() || args[0].rawValue.empty()) {
      const auto& current = ThemeManager::instance().currentTheme();
      const auto names = ThemeManager::instance().themeNames();
      app_.state_.addItem(std::make_unique<SystemNoticeItem>(
          "Theme: " + current.name));
      std::string joined = "Available themes:";
      for (const auto& name : names) joined += " " + name;
      app_.state_.addItem(std::make_unique<SystemNoticeItem>(joined));
      return;
    }
    app_.applyTheme(args[0].rawValue);
  }
private:
  App& app_;
};

App::App(AppOptions options)
    : options_(std::move(options)),
      eventRouter_(state_),
      dispatcher_(session_, state_),
      statusBar_(state_),
      agentTabBar_(state_),
      inputBar_(state_),
      bottomBar_(state_),
      notificationStrip_(notifications_) {}

App::~App() {
  if (terminal_.isActive()) {
    terminal_.showCursor();
    terminal_.leave();
  }
}

int App::run() {
  if (!terminal_.enter()) {
    ::fprintf(stderr, "Failed to initialize terminal.\n");
    return 1;
  }

  // Register tool presenters.
  registerAllPresenters();

  // Register commands and keybinds.
  registerCommands();
  setupKeybinds();

  // Set up pinned zone: statusBar + inputBar. Heights are dynamic.
  // bottomBar is rendered manually (after autocomplete rows when active).
  layout_.setPinnedComponents({&statusBar_, &inputBar_});

  // Paint the first frame immediately so the user sees the TUI while connecting.
  state_.setConnectionStatus(ConnectionStatus::Connecting);
  renderFrame();

  // Connect to daemon in the background so the main loop can paint frames
  // (init progress events arrive as SystemNoticeItems via the event loop).
  std::thread connectThread([this]() {
    connectAndSetup();
    state_.markDirtyPublic();
  });
  connectThread.detach();

  // Main loop.
  running_ = true;
  std::chrono::steady_clock::time_point lastLiveTick{};

  while (running_) {
    // Check for resize.
    if (terminal_.wasResized()) {
      onResize();
    }

    // Read input with mouse support (non-blocking).
    //
    // We bump the input-poll rate when something on the screen is moving
    // (live tool rows, status messages, an active turn) — otherwise the
    // user would see notification fades stutter or live spinners freeze
    // during long idle waits. Notification fade-ins/outs piggyback on
    // the same cadence.
    const auto loopNow = std::chrono::steady_clock::now();
    const bool notifAnimating = notifications_.needsAnimationTick(loopNow);
    const bool notifLive      = notifications_.hasLive(loopNow);
    const bool highRefreshInput =
        state_.hasLiveItems() || !state_.liveMessage().empty() ||
        state_.activityContext() == ActivityContext::Active ||
        notifAnimating;
    const int inputTimeoutMs = highRefreshInput ? 80 : 120;
    auto [key, mouseEvent] = terminal_.readInput(inputTimeoutMs);
    if (mouseEvent.has_value()) {
      handleMouse(mouseEvent.value());
    } else if (!key.empty()) {
      handleInput(key);
    }

    // Reconcile runtime state even if the final daemon event was missed.
    reconcileRuntimeState();

    // Drain anything that was queued from the daemon-event reader thread
    // (e.g. RewindApplied → reload transcript). These are safe to issue
    // RPCs from because we're on the main loop, not the reader.
    drainDeferredActions();

    // Garbage-collect expired notifications so the strip stops drawing
    // them and the stack never grows unbounded.
    notifications_.prune(std::chrono::steady_clock::now());

    // Live tick — keep tool presenters and status animations responsive.
    const bool animatePinnedUi =
        state_.hasLiveItems() || !state_.liveMessage().empty() ||
        state_.activityContext() == ActivityContext::Active ||
        notifLive;
    if (animatePinnedUi) {
      auto now = std::chrono::steady_clock::now();
      if (now - lastLiveTick > std::chrono::milliseconds(90)) {
        lastLiveTick = now;
        if (state_.hasLiveItems()) {
          state_.markLiveItemsDirty();
        } else {
          state_.markDirtyPublic();
        }
      }
    }

    // Sync new items to scrollback buffer.
    syncScrollback();

    // Re-render if state changed.
    if (state_.isDirty()) {
      renderFrame();
      state_.clearDirty();
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }

  terminal_.showCursor();
  terminal_.leave();
  return 0;
}

void App::registerCommands() {
  commands_.registerCommand(std::make_shared<QuitCmd>(*this));
  commands_.registerCommand(std::make_shared<NewCmd>(*this));
  commands_.registerCommand(std::make_shared<ModelsCmd>(*this));
  commands_.registerCommand(std::make_shared<ResumeCmd>(*this));
  commands_.registerCommand(std::make_shared<AccountsCmd>(*this));
  commands_.registerCommand(std::make_shared<ConnectCmd>(*this));
  commands_.registerCommand(std::make_shared<UndoCmd>(*this));
  commands_.registerCommand(std::make_shared<RedoCmd>(*this));
  commands_.registerCommand(std::make_shared<RouterCmd>(*this));
  commands_.registerCommand(std::make_shared<PurposesCmd>(*this));
  commands_.registerCommand(std::make_shared<PermissionsCmd>(*this));
  commands_.registerCommand(std::make_shared<ThemeCmd>(*this));

  // Load workflow definitions from disk and register as slash commands.
  firmius::core::WorkflowLoader::instance().init();
  registerWorkflowCommands(commands_, dispatcher_);
}

void App::setupKeybinds() {
  keybinds_.registerKeybind({keys::kCtrlQ, "Quit", ActivityContext::Idle, true,
                             [this]() { running_ = false; }});

  keybinds_.registerKeybind({keys::kCtrlC, "Quit", ActivityContext::Idle, true,
                             [this]() { running_ = false; }});
  keybinds_.registerKeybind(
      {keys::kCtrlC, "Interrupt", ActivityContext::Active, false,
       [this]() { dispatcher_.interruptAgent(); }});

  keybinds_.registerKeybind(
      {keys::kEscape, "Interrupt", ActivityContext::Active, false,
       [this]() { dispatcher_.interruptAgent(true); }});
  keybinds_.registerKeybind({keys::kEscape, "Dismiss", ActivityContext::Idle,
                             true, [this]() {
                               if (activeOverlay_) { dismissOverlay(); return; }
                               dismissAutocomplete();
                             }});

  // Agent focus navigation (active in both Idle and Active contexts)
  keybinds_.registerKeybind({keys::kCtrlN, "Next Agent", ActivityContext::Idle,
                             true, [this]() {
                               auto candidates = state_.siblingsOf(state_.focusedAgentId());
                               if (candidates.empty()) return;
                               auto it = std::find(candidates.begin(), candidates.end(),
                                                   state_.focusedAgentId());
                               size_t idx = (it == candidates.end()) ? 0 :
                                   (std::distance(candidates.begin(), it) + 1) % candidates.size();
                               switchToAgentTranscript(candidates[idx]);
                             }});
  keybinds_.registerKeybind({keys::kCtrlN, "Next Agent", ActivityContext::Active,
                             true, [this]() {
                               auto candidates = state_.siblingsOf(state_.focusedAgentId());
                               if (candidates.empty()) return;
                               auto it = std::find(candidates.begin(), candidates.end(),
                                                   state_.focusedAgentId());
                               size_t idx = (it == candidates.end()) ? 0 :
                                   (std::distance(candidates.begin(), it) + 1) % candidates.size();
                               switchToAgentTranscript(candidates[idx]);
                             }});
  keybinds_.registerKeybind({keys::kCtrlB, "Prev Agent", ActivityContext::Idle,
                             true, [this]() {
                               auto candidates = state_.siblingsOf(state_.focusedAgentId());
                               if (candidates.empty()) return;
                               auto it = std::find(candidates.begin(), candidates.end(),
                                                   state_.focusedAgentId());
                               size_t idx = (it == candidates.end()) ? 0 :
                                   (std::distance(candidates.begin(), it) + candidates.size() - 1)
                                   % candidates.size();
                               switchToAgentTranscript(candidates[idx]);
                             }});
  keybinds_.registerKeybind({keys::kCtrlB, "Prev Agent", ActivityContext::Active,
                             true, [this]() {
                               auto candidates = state_.siblingsOf(state_.focusedAgentId());
                               if (candidates.empty()) return;
                               auto it = std::find(candidates.begin(), candidates.end(),
                                                   state_.focusedAgentId());
                               size_t idx = (it == candidates.end()) ? 0 :
                                   (std::distance(candidates.begin(), it) + candidates.size() - 1)
                                   % candidates.size();
                               switchToAgentTranscript(candidates[idx]);
                             }});
  keybinds_.registerKeybind({keys::kCtrlP, "Parent Agent", ActivityContext::Idle,
                             true, [this]() {
                               auto parentId = state_.parentIdOf(state_.focusedAgentId());
                               if (!parentId.empty()) {
                                 switchToAgentTranscript(parentId);
                               }
                             }});
  keybinds_.registerKeybind({keys::kCtrlP, "Parent Agent", ActivityContext::Active,
                             true, [this]() {
                               auto parentId = state_.parentIdOf(state_.focusedAgentId());
                               if (!parentId.empty()) {
                                 switchToAgentTranscript(parentId);
                               }
                             }});

  keybinds_.registerKeybind({keys::kEnter, "Send", ActivityContext::Idle, false,
                             [this]() {
                               if (menuList_.isActive()) {
                                 menuList_.selectCurrent();
                                 return;
                               }
                               if (autocomplete_.active) {
                                 autocompleteAccept();
                                 return;
                               }
                               submitInputBuffer();
                             }});
  keybinds_.registerKeybind({keys::kEnter, "Send", ActivityContext::Active, false,
                             [this]() {
                               if (menuList_.isActive()) {
                                 menuList_.selectCurrent();
                                 return;
                               }
                               if (autocomplete_.active) {
                                 autocompleteAccept();
                                 return;
                               }
                               submitInputBuffer();
                             }});

  keybinds_.registerKeybind({keys::kUp, "Up", ActivityContext::Idle, true,
                             [this]() {
                               if (autocomplete_.active) {
                                 autocompleteMoveUp();
                                 return;
                               }
                             }});
  keybinds_.registerKeybind({keys::kDown, "Down", ActivityContext::Idle, true,
                             [this]() {
                               if (autocomplete_.active) {
                                 autocompleteMoveDown();
                                 return;
                               }
                             }});

  keybinds_.registerKeybind({keys::kBackspace, "Delete", ActivityContext::Idle,
                             true, [this]() {
                               state_.backspaceInput();
                               if (autocomplete_.active) {
                                 // Re-evaluate from scratch — input may now
                                 // be too short (no leading '/' anymore) or
                                 // we may have crossed back from arg-mode
                                 // into name-mode by deleting the space.
                                 std::string buf = state_.inputBuffer();
                                 if (buf.empty() || buf[0] != '/') {
                                   dismissAutocomplete();
                                 } else {
                                   autocomplete_.selectedIndex = 0;
                                   autocomplete_.scrollOffset = 0;
                                   updateAutocomplete();
                                 }
                               }
                             }});
  keybinds_.registerKeybind({keys::kBackspaceDel, "Delete", ActivityContext::Idle,
                             true, [this]() {
                               state_.backspaceInput();
                               if (autocomplete_.active) {
                                 std::string buf = state_.inputBuffer();
                                 if (buf.empty() || buf[0] != '/') {
                                   dismissAutocomplete();
                                 } else {
                                   autocomplete_.selectedIndex = 0;
                                   autocomplete_.scrollOffset = 0;
                                   updateAutocomplete();
                                 }
                               }
                             }});

  // Scroll keybinds — always active (user can scroll during streaming).
  auto [w, h] = terminal_.size();
  int halfPage = h / 2;

  keybinds_.registerKeybind({keys::kPageUp, "Scroll Up", ActivityContext::Idle,
                             true, [this, halfPage]() {
                               state_.scrollUp(halfPage);
                             }});
  keybinds_.registerKeybind({keys::kPageDown, "Scroll Down", ActivityContext::Idle,
                             true, [this, halfPage]() {
                               state_.scrollDown(halfPage);
                             }});
  keybinds_.registerKeybind({keys::kHome, "Top", ActivityContext::Idle,
                             true, [this]() { state_.scrollToTop(); }});
  keybinds_.registerKeybind({keys::kEnd, "Bottom", ActivityContext::Idle,
                             true, [this]() { state_.scrollToBottom(); }});

  // Expand/collapse tool output (Ctrl+E).
  keybinds_.registerKeybind({keys::kCtrlE, "Expand", ActivityContext::Idle,
                             true, [this]() {
                               auto* tc = state_.findLastFocusedToolCall();
                               if (tc) {
                                 tc->setExpanded(!tc->isExpanded());
                                 syncScrollback();
                                 state_.markDirtyPublic();
                               }
                             }});
  keybinds_.registerKeybind({keys::kCtrlT, "Todos", ActivityContext::Idle,
                             true, [this]() { state_.toggleTodoVisibility(); }});
  keybinds_.registerKeybind({keys::kCtrlT, "Todos", ActivityContext::Active,
                             true, [this]() { state_.toggleTodoVisibility(); }});

  keybinds_.registerKeybind({keys::kCtrlY, "Cycle Perm Mode", ActivityContext::Idle,
                             true, [this]() {
    // Cycle through the user's defined modes in order — same shape as
    // the /permissions overlay's mode picker. Wraps after the last.
    auto modes = state_.modes();
    if (modes.empty()) return;
    const auto active = state_.activeModeId();
    int idx = -1;
    for (int i = 0; i < static_cast<int>(modes.size()); ++i) {
      if (modes[i].id == active) { idx = i; break; }
    }
    int next = (idx + 1) % static_cast<int>(modes.size());
    try {
      firmius::daemon::PermissionModeUpdateRequest req;
      req.modeId = modes[next].id;
      auto snap = session_.client().setPermissionMode(req);
      state_.setActiveModeId(snap.activeModeId);
      notifications_.info("Permission mode: " + modes[next].name);
    } catch (const std::exception& e) {
      notifications_.error(std::string("Failed to set permission mode: ") + e.what());
    }
  }});
}

void App::handleInput(const std::string& key) {
  // ── Bracketed paste ──────────────────────────────────────────────────
  // Always check for paste markers before anything else, so a paste during
  // an active agent run doesn't get re-interpreted as Esc-to-interrupt.
  if (key == "\x1b[200~") {
    inBracketedPaste_ = true;
    pasteBuffer_.clear();
    return;
  }
  if (key == "\x1b[201~") {
    if (inBracketedPaste_) {
      commitPasteBuffer();
    }
    inBracketedPaste_ = false;
    pasteBuffer_.clear();
    return;
  }
  if (inBracketedPaste_) {
    // Accumulate raw bytes between paste markers. The terminal sends them
    // in arbitrary chunks; we buffer everything until we see the end
    // marker, then decide text vs. image.
    pasteBuffer_ += key;
    return;
  }

  // ── Kitty keyboard protocol → classic key translation ─────────────────
  //
  // We enable the kitty disambiguation flag in Terminal::enter so we can
  // distinguish Shift+Enter from plain Enter. Side effect: terminals
  // that honour the flag (kitty, alacritty, wezterm, foot, ghostty,
  // recent gnome-terminal) send EVERY key as a CSI-u sequence,
  // including bare Enter and Ctrl+letter combos. Without translation,
  // downstream handlers that expect "\r" or "\x11" (Ctrl+Q) silently
  // miss the keystroke.
  //
  // Format: "\x1b[<keycode>[;<modifier>]u"
  //   keycode = unicode codepoint of the unshifted key
  //   modifier = bitmask + 1: shift=1, alt=2, ctrl=4, super=8 (so a value
  //              of 5 means "ctrl" alone — modifier 5 = (4 ctrl) + 1)
  //
  // We translate the common cases back to their classic byte form so
  // every other handler keeps working. Sequences we don't recognise
  // (function keys, multi-modifier combos) drop through to the
  // unrecognised-ESC guard later.
  std::string translatedKey;
  if (key.size() >= 4 && key[0] == '\x1b' && key[1] == '[' &&
      key.back() == 'u') {
    // Parse "<keycode>[;<modifier>]" between '[' and 'u'.
    int code = 0;
    int mod = 1;  // 1 = no modifiers
    bool inMod = false;
    bool ok = true;
    for (size_t i = 2; i + 1 < key.size(); ++i) {
      const char c = key[i];
      if (c >= '0' && c <= '9') {
        if (inMod) mod = mod * 10 + (c - '0');
        else       code = code * 10 + (c - '0');
      } else if (c == ';' && !inMod) {
        inMod = true;
        mod = 0;  // reset accumulator now that we're parsing it
      } else {
        ok = false;
        break;
      }
    }
    if (ok && code > 0) {
      const bool ctrl  = (mod - 1) & 4;
      const bool alt   = (mod - 1) & 2;
      const bool shift = (mod - 1) & 1;
      // Classic forms we care about. Anything else falls through with
      // translatedKey empty — unrecognised CSI-u → drop later.
      if (code == 13 && shift && !ctrl && !alt) {
        translatedKey = "\x1b\r";       // Shift+Enter → Alt+Enter form
      } else if (code == 13 && !shift && !ctrl && !alt) {
        translatedKey = "\r";           // bare Enter
      } else if (code == 27 && !ctrl && !alt) {
        translatedKey = "\x1b";         // bare Esc
      } else if (code == 9 && !ctrl && !alt) {
        translatedKey = "\t";           // Tab
      } else if (code == 127 && alt && !ctrl) {
        translatedKey = "\x1b\x7f";     // Alt+Backspace
      } else if (code == 127 && !alt && !ctrl) {
        translatedKey = "\x7f";         // Backspace
      } else if (ctrl && !alt && !shift && code >= 'a' && code <= 'z') {
        // Ctrl+letter → byte 1..26 (e.g. Ctrl+Q = 0x11, Ctrl+C = 0x03,
        // Ctrl+V = 0x16, Ctrl+N = 0x0E, etc.)
        translatedKey = std::string(1, static_cast<char>(code - 'a' + 1));
      } else if (code >= 32 && code < 127 && !ctrl && !alt) {
        // Unmodified printable ASCII — kitty wraps even these in CSI-u
        // when the disambiguation flag is set. Surface the byte so
        // appendToInput() / autocomplete sees it normally.
        translatedKey = std::string(1, static_cast<char>(code));
      }
    }
  }
  // If we translated, recurse with the classic form so every downstream
  // check (which is keyed on classic byte sequences) Just Works without
  // peppering effKey checks throughout this function.
  if (!translatedKey.empty() && translatedKey != key) {
    handleInput(translatedKey);
    return;
  }

  auto context = state_.activityContext();

  // Bare ESC during an active turn = interrupt. Multi-byte sequences
  // starting with ESC (arrow keys, alt-modifiers, etc.) flow through to
  // normal handling so the user can still navigate the input bar mid-run.
  if (context == ActivityContext::Active && key.size() == 1 &&
      static_cast<unsigned char>(key[0]) == 0x1b) {
    dispatcher_.interruptAgent(true);
    return;
  }

  if (context == ActivityContext::PermissionPending) {
    // Auto-open the prompt overlay for the front of the queue; subsequent
    // keys are routed via activeOverlay_->handleInput below.
    if (activeOverlay_ != &permissionOverlay_) {
      openPermissionPromptOverlay();
    }
    if (activeOverlay_ == &permissionOverlay_) {
      activeOverlay_->handleInput(key);
      return;
    }
    return;
  }

  if (!state_.daemonReady()) {
    keybinds_.handleKey(key, context);
    return;
  }

  // When overlay is active, delegate input to it.
  if (activeOverlay_) {
    if (activeOverlay_->handleInput(key)) {
      state_.markDirtyPublic();
    }
    return;
  }

  // Autocomplete navigation.
  if (autocomplete_.active) {
    if (key == "\x1b") {
      dismissAutocomplete();
      return;
    }
    if (key == "\t") {
      autocompleteAccept();
      return;
    }
    if (key == "\r" || key == "\n") {
      // Two cases for Enter while autocomplete is open:
      //   (a) The user is mid-pick — accept the highlighted suggestion.
      //   (b) The user has fully typed what they want and the highlight
      //       is just a "you'd match this if you accepted" hint. In that
      //       case we should SUBMIT, not loop forever appending spaces.
      //
      // Heuristic: if there are no matches at all, or the current filter
      // exactly equals the selected match's name, Enter means submit.
      // Otherwise it accepts the highlighted suggestion.
      bool submitInstead = false;
      if (autocomplete_.matches.empty()) {
        submitInstead = true;
      } else if (autocomplete_.mode == AutocompleteState::Mode::ArgValue) {
        const auto& match =
            autocomplete_.matches[autocomplete_.selectedIndex];
        if (autocomplete_.argFilter == match.name) {
          submitInstead = true;
        }
      }
      // (For command-name mode we always accept — Enter on "/conn"
      //  picking the highlighted "/connect" suggestion is the expected
      //  shortcut. Submitting a fully-typed command name with no args
      //  also goes through autocompleteAccept, which dismisses.)
      if (submitInstead) {
        dismissAutocomplete();
        submitInputBuffer();
      } else {
        autocompleteAccept();
      }
      return;
    }
    if (key == keys::kUp || key == "\x1b[A") {
      autocompleteMoveUp();
      return;
    }
    if (key == keys::kDown || key == "\x1b[B") {
      autocompleteMoveDown();
      return;
    }
    // Printable chars fall through to normal input handling to update prefix.
  }

  // ── Input editing keys ─────────────────────────────────────────────────
  // Cursor movement, word navigation, line breaks, and word delete. These
  // run before keybinds_ so they take precedence over any global bindings
  // on the same sequences (e.g. Up/Down for autocomplete already handled).
  //
  // Newline insertion: we accept BOTH Alt+Enter (`\x1b\r`) and the kitty-
  // protocol Shift+Enter (`\x1b[13;2u`). xterm without modifyOtherKeys
  // can't distinguish Shift+Enter from plain Enter — those users get
  // Alt+Enter as the documented way to insert a literal newline.
  if (!key.empty()) {
    // Cursor movement.
    if (key == "\x1b[D") {  // Left
      state_.moveCursorLeft();
      return;
    }
    if (key == "\x1b[C") {  // Right
      state_.moveCursorRight();
      return;
    }
    if (key == "\x1b[A") {  // Up
      state_.moveCursorUp();
      return;
    }
    if (key == "\x1b[B") {  // Down
      state_.moveCursorDown();
      return;
    }
    // Word navigation: Ctrl+Left / Ctrl+Right.
    // xterm: \x1b[1;5D / \x1b[1;5C   (modifier 5 = Ctrl)
    // Some terminals also send \x1b[5D / \x1b[5C — accept both.
    if (key == "\x1b[1;5D" || key == "\x1b[5D" || key == "\x1b[1;3D") {
      state_.moveCursorWordLeft();
      return;
    }
    if (key == "\x1b[1;5C" || key == "\x1b[5C" || key == "\x1b[1;3C") {
      state_.moveCursorWordRight();
      return;
    }
    // Home / End.
    if (key == "\x1b[H" || key == "\x1b[1~") {
      state_.moveCursorLineStart();
      return;
    }
    if (key == "\x1b[F" || key == "\x1b[4~") {
      state_.moveCursorLineEnd();
      return;
    }
    // Alt+Backspace: delete previous word.
    // Classic: \x1b\x7f or \x1b\b
    // Kitty protocol: \x1b[127;3u (key=127, modifier=3=Alt)
    if (key == "\x1b\x7f" || key == "\x1b\b" || key == "\x1b[127;3u") {
      state_.deleteWordBeforeCursor();
      if (autocomplete_.active) {
        if (state_.inputBuffer().empty() ||
            state_.inputBuffer()[0] != '/') {
          dismissAutocomplete();
        } else {
          autocomplete_.selectedIndex = 0;
          autocomplete_.scrollOffset = 0;
          updateAutocomplete();
        }
      }
      return;
    }
    // Newline insertion: Alt+Enter or kitty Shift+Enter.
    if (key == "\x1b\r" || key == "\x1b\n" || key == "\x1b[13;2u") {
      state_.insertAtCursor("\n");
      return;
    }
    // Ctrl+V: image paste from system clipboard. Bracketed paste already
    // handles plain text — Ctrl+V is the path users expect for images,
    // since most terminals don't expose image bytes through bracketed
    // paste at all.
    if (key == "\x16") {
      std::string mime;
      auto base64 = Clipboard::getImage(mime);
      if (base64.has_value() && !base64->empty()) {
        state_.insertPastedImage(std::move(*base64), std::move(mime));
        notifications_.success("Pasted image", "clipboard");
      } else {
        notifications_.warning("No image on clipboard", "clipboard");
      }
      state_.markDirtyPublic();
      return;
    }
    // Kitty Escape: \x1b[27u — treat as bare Esc.
    if (key == "\x1b[27u") {
      // In Active context, interrupt. Otherwise, dismiss autocomplete or
      // clear selection — same as bare Esc.
      if (context == ActivityContext::Active) {
        dispatcher_.interruptAgent(true);
      } else if (autocomplete_.active) {
        dismissAutocomplete();
      } else if (state_.hasSelection()) {
        state_.clearSelection();
      }
      return;
    }
    // ── Drop any other unrecognized escape sequence ──────────────────
    // If the key starts with ESC and we haven't handled it above, it's a
    // control sequence we don't support (e.g. \x1b[1;3A = Alt+Up). Do NOT
    // let it fall through to the printable-char loop — that would leak the
    // raw bytes into the input buffer.
    if (key[0] == '\x1b' && key.size() > 1) {
      return;
    }
  }

  if (keybinds_.handleKey(key, context)) {
    return;
  }

  for (unsigned char ch : key) {
    if (ch == '\r' || ch == '\n' || ch == 0x7f || ch == '\b') {
      keybinds_.handleKey(std::string(1, static_cast<char>(ch)), context);
    } else if (ch >= 32 && ch != 0x7f) {
      // Accept printable ASCII (32-126) AND UTF-8 continuation/lead bytes
      // (>= 0x80). Multi-byte UTF-8 chars arrive as raw bytes in `key` —
      // we just append them and let the renderer handle width.
      state_.appendToInput(static_cast<char>(ch));
      // Trigger autocomplete if input starts with '/'.
      if (state_.inputBuffer().size() == 1 && ch == '/') {
        autocomplete_.active = true;
        autocomplete_.selectedIndex = 0;
        autocomplete_.scrollOffset = 0;
        updateAutocomplete();
      } else if (autocomplete_.active) {
        // Update autocomplete as user types. Don't dismiss on space — that's
        // the trigger for arg autocomplete.
        autocomplete_.selectedIndex = 0;
        autocomplete_.scrollOffset = 0;
        updateAutocomplete();
      }
    }
  }
}

void App::handleMouse(const MouseEvent& event) {
  auto [w, h] = terminal_.size();
  const int liveH = statusBar_.liveHeight(w);
  const int overlayH = activeOverlay_ ? activeOverlay_->height(w) : 0;
  const int inputH = inputBar_.height(w);
  const int agentH = state_.hasMultipleAgents() ? agentTabBar_.height(w) : 0;
  const int hudH = statusBar_.hudHeight(w);
  const int bottomH = bottomBar_.height(w);
  const int pinnedH = liveH + overlayH + inputH + agentH + hudH + bottomH;

  if (activeOverlay_) {
    int overlayStartRow1 = h - pinnedH + liveH + 1;
    bool inside = event.row >= overlayStartRow1 &&
                  event.row < overlayStartRow1 + overlayH;
    if (inside) {
      if (activeOverlay_ == &menuList_) {
        menuList_.setScreenRow(overlayStartRow1);
      }
      if (activeOverlay_->handleMouse(event, event.row, event.col)) {
        state_.markDirtyPublic();
      }
      return;
    }
    if (event.type == MouseEvent::Type::Scroll) {
      if (event.button == MouseEvent::Button::ScrollUp) {
        state_.scrollUp(3);
      } else if (event.button == MouseEvent::Button::ScrollDown) {
        state_.scrollDown(3);
      }
    }
    return;
  }

  // ── No overlay: transcript zone owns mouse interactions ────────────────
  //
  // The transcript renders rows [0, transcriptH) at the top of the screen.
  // The scrollback is a flat std::vector; line `i` of scrollback maps to
  // visual row `i - startLine` where startLine accounts for scroll offset.
  // For selection we convert (mouse_row, mouse_col) → (absolute scrollback
  // line, byte column) so the selection survives scrolling.

  const int transcriptH = std::max(1, h - pinnedH);
  // 0-indexed visual row of the mouse inside the transcript zone.
  const int mouseVisRow = event.row - 1;  // event.row is 1-indexed
  const bool inTranscript = (mouseVisRow >= 0 && mouseVisRow < transcriptH);

  // Helper: visual row → absolute scrollback line.
  auto absLineForVisRow = [&](int visRow) -> int {
    const int total = state_.scrollbackSize();
    const int offset = state_.scrollOffset();
    const int startLine = std::max(0, total - transcriptH - offset);
    return startLine + visRow;
  };

  if (event.type == MouseEvent::Type::Scroll) {
    // While selecting, scroll wheel extends the selection vertically. This
    // matches the user's request: "drag mouse + scroll up reveals more text
    // and the selection follows." Without holding the button, scroll is
    // just normal transcript scrolling.
    int amount = (event.button == MouseEvent::Button::ScrollUp) ? 3 : -3;
    if (amount > 0) {
      state_.scrollUp(amount);
    } else {
      state_.scrollDown(-amount);
    }
    if (state_.isSelecting()) {
      // After scrolling, recompute the absolute line under the cursor's
      // current row (we don't know exactly where the mouse is after a
      // scroll-only event, so use the last known row capped to the zone).
      int row = std::clamp(mouseVisRow, 0, transcriptH - 1);
      state_.updateSelection(absLineForVisRow(row),
                             std::max(0, event.col - 1));
    }
    return;
  }

  // Left-button press inside transcript begins a selection; click outside
  // (or on a non-left button) clears any existing selection.
  if (event.type == MouseEvent::Type::Press) {
    if (inTranscript && event.button == MouseEvent::Button::Left) {
      const int absLine = absLineForVisRow(mouseVisRow);
      const int col = std::max(0, event.col - 1);
      state_.beginSelection(absLine, col);
    } else if (state_.hasSelection()) {
      state_.clearSelection();
    }
    return;
  }

  // Drag (mouse moved with button held). Update selection cursor and, if
  // the mouse went above/below the transcript zone, auto-scroll to keep
  // the cursor "alive". This is what lets the user drag past the top of
  // the visible window and have the transcript scroll up under them.
  if (event.type == MouseEvent::Type::Move &&
      event.button == MouseEvent::Button::Left &&
      state_.isSelecting()) {
    int row = mouseVisRow;
    if (row < 0) {
      // Mouse above the zone: scroll up and clamp the row to 0.
      state_.scrollUp(1);
      row = 0;
    } else if (row >= transcriptH) {
      // Mouse below the zone (in the pinned area): scroll down.
      state_.scrollDown(1);
      row = transcriptH - 1;
    }
    const int absLine = absLineForVisRow(row);
    const int col = std::max(0, event.col - 1);
    state_.updateSelection(absLine, col);
    return;
  }

  // Release ends the drag. If the result is a non-empty range, copy it to
  // the clipboard right away — that's the user's expectation: "drag, let
  // go, paste".
  if (event.type == MouseEvent::Type::Release &&
      event.button == MouseEvent::Button::Left) {
    const bool wasSelecting = state_.isSelecting();
    state_.endSelection();
    if (wasSelecting && state_.hasSelection()) {
      const std::string text = state_.copySelectedText();
      if (!text.empty()) {
        if (Clipboard::setText(text)) {
          // Dedupe key keeps repeat-drag from stacking N toasts during
          // a fast multi-select session — the same toast just refreshes.
          notifications_.success(
              "Copied " + std::to_string(text.size()) +
                  (text.size() == 1 ? " char" : " chars"),
              "clipboard");
        }
      }
    }
    return;
  }
}

void App::reconcileRuntimeState() {
  const auto status = state_.agentStatus();
  if (status == firmius::shared::AgentStatus::Idle ||
      status == firmius::shared::AgentStatus::Cancelled ||
      status == firmius::shared::AgentStatus::Error) {
    return;
  }

  const auto now = std::chrono::steady_clock::now();
  if (lastRuntimeReconcile_ != std::chrono::steady_clock::time_point{} &&
      now - lastRuntimeReconcile_ < std::chrono::milliseconds(500)) {
    return;
  }
  lastRuntimeReconcile_ = now;

  const auto threadId = state_.threadId();
  const auto agentId = state_.focusedAgentId();
  if (threadId.empty() || agentId.empty()) return;

  try {
    auto agent = session_.getAgent(threadId, agentId);
    if (!agent) return;

    state_.setModelLabel(agent->providerId + "/" + agent->modelId);
    if (!agent->running && !agent->booting &&
        agent->status != firmius::shared::AgentStatus::Streaming &&
        agent->status != firmius::shared::AgentStatus::ProviderWaiting &&
        agent->status != firmius::shared::AgentStatus::ExecutingTool) {
      // Finalize active streaming items
      auto* textItem = state_.activeTextItem();
      if (textItem && !textItem->isFinalized()) {
        textItem->finalize();
      }
      state_.setActiveTextItem(nullptr);

      auto* thinkItem = state_.activeThinkingItem();
      if (thinkItem && !thinkItem->isFinalized()) {
        thinkItem->finalize();
      }
      state_.setActiveThinkingItem(nullptr);

      state_.setAgentStatus(firmius::shared::AgentStatus::Idle);
      if (state_.queuedMessageCount() > 0) {
        dispatcher_.loadTranscriptForAgent(agentId, true);
        state_.setQueuedMessageCount(0);
      }
    }
  } catch (const std::exception&) {
    // Keep the UI responsive if the daemon disconnects mid-stream.
  }
}

// ── Scrollback Management ──

namespace {

bool shouldShowItem(const TranscriptItem& item, const std::string& focusedAgentId) {
  auto type = item.type();
  // User messages: per-agent. Show if agentId matches focused, or if no agentId (legacy)
  if (type == "UserMessage") {
    const auto& um = static_cast<const UserMessageItem&>(item);
    return um.agentId().empty() || um.agentId() == focusedAgentId;
  }
  // System/error notices show for all agents
  if (type == "SystemNotice" || type == "ErrorMessage") {
    return true;
  }
  // Agent-scoped items: check agentId
  if (type == "AgentText") {
    const auto& textItem = static_cast<const AgentTextItem&>(item);
    return textItem.agentId().empty() || textItem.agentId() == focusedAgentId;
  }
  if (type == "AgentThinking") {
    const auto& thinkItem = static_cast<const AgentThinkingItem&>(item);
    return thinkItem.agentId().empty() || thinkItem.agentId() == focusedAgentId;
  }
  if (type == "ToolCall") {
    const auto& tcItem = static_cast<const ToolCallItem&>(item);
    return tcItem.agentId().empty() || tcItem.agentId() == focusedAgentId;
  }
  return true; // Unknown types: show by default
}

} // namespace

void App::syncScrollback() {
  const auto& items = state_.items();
  int w = terminal_.size().first;
  std::string focusedId = state_.focusedAgentId();

  if (state_.consumeFullResyncRequested()) {
    state_.clearScrollback();
    lastSyncedItemCount_ = 0;
    lastSyncedRowCounts_.clear();
  }

  // If focus changed, rebuild everything from scratch
  if (focusedId != lastFocusedAgentId_) {
    lastFocusedAgentId_ = focusedId;
    state_.clearScrollback();
    lastSyncedItemCount_ = 0;
    lastSyncedRowCounts_.clear();
    // Fall through to re-sync everything
  }

  // Find the first dirty item (or any item beyond what we've synced).
  size_t rebuildFrom = items.size();

  if (lastSyncedItemCount_ < items.size()) {
    rebuildFrom = lastSyncedItemCount_;
  }

  // Check for dirty items that were already synced.
  for (size_t i = 0; i < lastSyncedItemCount_ && i < items.size(); ++i) {
    if (items[i]->needsRender()) {
      rebuildFrom = std::min(rebuildFrom, i);
      break;
    }
  }

  if (rebuildFrom >= items.size()) return;

  // Remove scrollback lines from the first dirty item onward.
  int linesToRemove = 0;
  for (size_t i = rebuildFrom; i < lastSyncedRowCounts_.size(); ++i) {
    linesToRemove += lastSyncedRowCounts_[i];
  }
  if (linesToRemove > 0) {
    state_.removeTrailingScrollback(linesToRemove);
  }

  // Re-render from the first dirty item, filtering by focused agent.
  std::vector<std::string> newLines;
  for (size_t i = rebuildFrom; i < items.size(); ++i) {
    // Filter: skip items that don't belong to the focused agent
    if (!shouldShowItem(*items[i], focusedId)) {
      // Record 0 rows for filtered items so index tracking stays valid
      if (i >= lastSyncedRowCounts_.size()) {
        lastSyncedRowCounts_.resize(i + 1, 0);
      }
      lastSyncedRowCounts_[i] = 0;
      if (items[i]->isFinalized()) {
        items[i]->markClean();
      }
      continue;
    }

    auto lines = items[i]->render(w);
    for (auto& line : lines) {
      newLines.push_back(std::move(line));
    }
    if (i >= lastSyncedRowCounts_.size()) {
      lastSyncedRowCounts_.resize(i + 1, 0);
    }
    lastSyncedRowCounts_[i] = static_cast<int>(lines.size());
    if (items[i]->isFinalized()) {
      items[i]->markClean();
    }
  }
  lastSyncedItemCount_ = items.size();

  if (!newLines.empty()) {
    state_.appendScrollback(newLines);
  }
}

void App::switchToAgentTranscript(const std::string& agentId) {
  state_.focusAgent(agentId);
  dispatcher_.loadTranscriptForAgent(agentId, true);
  // Force scrollback rebuild
  lastFocusedAgentId_ = agentId;
  state_.clearScrollback();
  lastSyncedItemCount_ = 0;
  lastSyncedRowCounts_.clear();
  state_.scrollToBottom();

  // Re-point global active streaming items to the new agent's items
  // (per-agent pointers are maintained by EventRouter automatically)
  auto* textItem = state_.agentTextItem(agentId);
  state_.setActiveTextItem(textItem && !textItem->isFinalized() ? textItem : nullptr);

  auto* thinkItem = state_.agentThinkingItem(agentId);
  state_.setActiveThinkingItem(thinkItem && !thinkItem->isFinalized() ? thinkItem : nullptr);
}

// ── Full-Screen Rendering Pipeline ──

void App::renderFrame() {
  auto [w, h] = terminal_.size();

  // Autocomplete dropdown height (0 when inactive). Capped at
  // kAutocompleteVisibleRows; navigation past the window scrolls the slice.
  int autocompleteH =
      autocomplete_.active && !autocomplete_.matches.empty()
          ? std::min(kAutocompleteVisibleRows,
                     static_cast<int>(autocomplete_.matches.size()))
          : 0;

  const auto liveLines = statusBar_.renderLiveSection(w);
  const auto notifLines = notificationStrip_.render(w);
  const auto overlayLines =
      activeOverlay_ ? activeOverlay_->render(w) : std::vector<std::string>{};
  const auto inputLines = inputBar_.render(w);
  const auto agentLines = state_.hasMultipleAgents() ? agentTabBar_.render(w)
                                                     : std::vector<std::string>{};
  const auto hudLines = statusBar_.renderHudSection(w);
  const auto bottomLines = bottomBar_.render(w);

  std::vector<std::string> pinnedLines;
  pinnedLines.reserve(liveLines.size() + notifLines.size() +
                      overlayLines.size() + inputLines.size() +
                      agentLines.size() + hudLines.size() + bottomLines.size());
  // Notifications sit at the very top of the pinned zone — directly under
  // the transcript — so they read like a stack of fading rows pushed up
  // by the live row beneath them. Newest at the bottom of THIS slice
  // (NotificationStrip already orders oldest→newest), which puts the
  // most recent toast right above the live row, where the eye lands.
  pinnedLines.insert(pinnedLines.end(), notifLines.begin(), notifLines.end());
  pinnedLines.insert(pinnedLines.end(), liveLines.begin(), liveLines.end());
  pinnedLines.insert(pinnedLines.end(), overlayLines.begin(), overlayLines.end());
  pinnedLines.insert(pinnedLines.end(), inputLines.begin(), inputLines.end());
  pinnedLines.insert(pinnedLines.end(), agentLines.begin(), agentLines.end());
  pinnedLines.insert(pinnedLines.end(), hudLines.begin(), hudLines.end());
  pinnedLines.insert(pinnedLines.end(), bottomLines.begin(), bottomLines.end());

  int basePinnedH = static_cast<int>(pinnedLines.size());
  int pinnedH = basePinnedH + autocompleteH;

  // Render transcript shorter to leave room for autocomplete + pinned zone.
  int transcriptH = h - pinnedH;
  if (transcriptH < 1) transcriptH = 1;

  CellGrid target(h, std::vector<Cell>(w));
  const auto& theme = ThemeManager::instance().currentTheme();
  for (int row = 0; row < h; ++row) {
    for (int col = 0; col < w; ++col) {
      target[row][col].bg.type = CellColor::Type::RGB;
      target[row][col].bg.r = static_cast<uint8_t>(theme.chat.bg.r);
      target[row][col].bg.g = static_cast<uint8_t>(theme.chat.bg.g);
      target[row][col].bg.b = static_cast<uint8_t>(theme.chat.bg.b);
    }
  }
  renderTranscriptZone(target, w, transcriptH);

  // Draw autocomplete rows directly under the transcript. We render the
  // window starting at `scrollOffset` — selectedIndex stays in the visible
  // window (clamping is done in updateAutocomplete / autocompleteMoveDown).
  if (autocompleteH > 0) {
    const int total = static_cast<int>(autocomplete_.matches.size());
    int acStart = transcriptH;
    for (int i = 0; i < autocompleteH; ++i) {
      int matchIdx = autocomplete_.scrollOffset + i;
      if (matchIdx < 0 || matchIdx >= total) break;
      int row = acStart + i;
      if (row < 0 || row >= h) continue;
      const auto& match = autocomplete_.matches[matchIdx];
      bool selected = (matchIdx == autocomplete_.selectedIndex);
      std::string line = "  " + match.name;
      if (!match.description.empty()) {
        line += "  " + ansi::dim(match.description);
      }
      // Append a "more" hint on the last visible row when the list extends
      // beyond the window in either direction.
      bool isFirstVisible = (i == 0);
      bool isLastVisible = (i == autocompleteH - 1);
      const bool hasMoreAbove = autocomplete_.scrollOffset > 0;
      const bool hasMoreBelow =
          autocomplete_.scrollOffset + autocompleteH < total;
      if (isFirstVisible && hasMoreAbove) {
        line += "  " + ansi::dim("↑ more");
      } else if (isLastVisible && hasMoreBelow) {
        line += "  " + ansi::dim("↓ " + std::to_string(total -
                                       (autocomplete_.scrollOffset +
                                        autocompleteH)) +
                                  " more");
      }
      if (selected) {
        line = theme_ansi::selection(ansi::fitToWidth(line, w));
      } else {
        line = ansi::fitToWidth(line, w);
      }
      auto cells = AnsiParser::parse(line, w);
      for (int c = 0; c < std::min(static_cast<int>(cells.size()), w); ++c) {
        applyFallbackBackground(cells[c], theme);
        blendCell(target[row][c], cells[c]);
      }
    }
  }

  // Draw pinned lines under autocomplete.
  int pinnedStart = transcriptH + autocompleteH;
  for (int i = 0; i < std::min(basePinnedH, h - pinnedStart); ++i) {
    int row = pinnedStart + i;
    if (row < 0 || row >= h) continue;
    auto cells = AnsiParser::parse(pinnedLines[static_cast<std::size_t>(i)], w);
    for (int c = 0; c < std::min(static_cast<int>(cells.size()), w); ++c) {
      applyFallbackBackground(cells[c], theme);
      blendCell(target[row][c], cells[c]);
    }
  }

  // Diff against previous frame and emit only changed cells.
  terminal_.beginBatch();
  terminal_.hideCursor();
  diffAndEmit(target, w, h);

  // Position cursor on the input line, not the separator.
  //
  // ANSI CUP (`\x1b[r;cH`) is 1-indexed. Our row math is 0-indexed, so we
  // add 1. The input line starts with a 3-cell prompt (" ❯ "), then the
  // cursor sits at column `1 + 3 + visualColumn` (1-indexed). For
  // multi-line / wrapped input we use cursorVisualRowFor/cursorVisualColumnFor
  // which both account for soft-wrap and the inner-bar scroll window.
  //
  // Order matches the pinned-zone composition above:
  //   notifLines → liveLines → overlayLines → inputLines → ...
  {
    const int cursorRow0 = pinnedStart +
                           static_cast<int>(notifLines.size()) +
                           static_cast<int>(liveLines.size()) +
                           static_cast<int>(overlayLines.size()) +
                           inputBar_.cursorRowOffset() +
                           inputBar_.cursorVisualRowFor(w);
    const int cursorCol0 = 3 + inputBar_.cursorVisualColumnFor(w);
    terminal_.moveCursor(cursorRow0 + 1, cursorCol0 + 1);
  }
  terminal_.showCursor();
  terminal_.flushBatch();

  // Save frame for next diff.
  prevFrame_ = std::move(target);
  prevW_ = w;
  prevH_ = h;
}

void App::renderTranscriptZone(CellGrid& target, int w, int transcriptH) {
  const auto& theme = ThemeManager::instance().currentTheme();
  const auto& scrollback = state_.scrollback();
  int totalLines = static_cast<int>(scrollback.size());
  int offset = state_.scrollOffset();

  // Compute visible range.
  int startLine = std::max(0, totalLines - transcriptH - offset);
  int endLine = std::min(totalLines, startLine + transcriptH);

  // Selection range is in absolute scrollback indices, so it survives
  // scrolling. We resolve it once per render and apply inversion to cells
  // inside the range.
  const auto [selStart, selEnd] = state_.selectionRange();
  const bool selValid = (selStart.line >= 0 && selEnd.line >= 0 &&
                         !(selStart == selEnd));

  // Fill target grid with parsed ANSI cells.
  for (int i = startLine; i < endLine; ++i) {
    int row = i - startLine; // 0-indexed row in target
    if (row >= transcriptH) break;
    auto cells = AnsiParser::parse(scrollback[i], w);
    for (int c = 0; c < std::min(static_cast<int>(cells.size()), w); ++c) {
      applyFallbackBackground(cells[c], theme);
      // Selection: invert cells whose absolute (line, col) is inside the
      // ordered range. We're working in byte-columns of the parsed cell
      // grid, which matches what selectionRange() reports.
      if (selValid) {
        bool inside = false;
        if (i > selStart.line && i < selEnd.line) {
          inside = true;
        } else if (i == selStart.line && i == selEnd.line) {
          inside = (c >= selStart.col && c < selEnd.col);
        } else if (i == selStart.line) {
          inside = (c >= selStart.col);
        } else if (i == selEnd.line) {
          inside = (c < selEnd.col);
        }
        if (inside) {
          cells[c].style.invert = true;
        }
      }
      blendCell(target[row][c], cells[c]);
    }
  }

  // Scroll indicator when not at bottom.
  if (offset > 0 && transcriptH > 0) {
    std::string indicator = " -- More (PgUp/Scroll) -- ";
    int indLen = static_cast<int>(indicator.size());
    int startCol = std::max(0, w - indLen - 1);
    auto indCells = AnsiParser::parse(
        "\x1b[2m" + indicator + "\x1b[22m", indLen);
    for (int c = 0; c < std::min(static_cast<int>(indCells.size()), w - startCol); ++c) {
      applyFallbackBackground(indCells[c], theme);
      blendCell(target[0][startCol + c], indCells[c]);
    }
  }
}

void App::renderPinnedZone(CellGrid& target, int w, int h, int pinnedH) {
  if (pinnedH <= 0) return;

  auto pinnedLines = layout_.renderPinned(w);
  int startRow = h - pinnedH; // 0-indexed

  for (int i = 0; i < std::min(static_cast<int>(pinnedLines.size()), pinnedH); ++i) {
    int row = startRow + i;
    if (row < 0 || row >= h) continue;
    auto cells = AnsiParser::parse(pinnedLines[i], w);
    for (int c = 0; c < std::min(static_cast<int>(cells.size()), w); ++c) {
      target[row][c] = cells[c];
    }
  }
}

void App::diffAndEmit(const CellGrid& target, int w, int h) {
  // If dimensions changed, force full redraw.
  if (w != prevW_ || h != prevH_) {
    prevFrame_.clear();
  }

  bool hasPrev = !prevFrame_.empty() &&
                 static_cast<int>(prevFrame_.size()) == h &&
                 static_cast<int>(prevFrame_[0].size()) == w;

  // Track current cursor position and style to minimize escape sequences.
  int curRow = -1, curCol = -1;
  CellColor curFg, curBg;
  CellStyle curStyle;
  bool styleValid = false;

  auto emitSgr = [&](const Cell& cell) {
    std::string sgr;

    // Build SGR sequence for changed attributes.
    if (!styleValid || curFg != cell.fg || curBg != cell.bg || curStyle != cell.style) {
      // Reset if style went from complex to different complex.
      bool needReset = styleValid && (curStyle != cell.style);
      if (needReset) {
        sgr += "\x1b[0m";
        // Re-apply active attributes.
        if (cell.style.bold) sgr += "\x1b[1m";
        if (cell.style.dim) sgr += "\x1b[2m";
        if (cell.style.italic) sgr += "\x1b[3m";
        if (cell.style.underline) sgr += "\x1b[4m";
        if (cell.style.strikethrough) sgr += "\x1b[9m";
        if (cell.style.invert) sgr += "\x1b[7m";
        // \x1b[0m resets ALL attributes including fg/bg to terminal default.
        // Invalidate cached colors so the checks below always re-apply them.
        curFg = {};
        curBg = {};
      } else {
        // Only emit changed style attributes.
        if (!styleValid || curStyle.bold != cell.style.bold) {
          sgr += cell.style.bold ? "\x1b[1m" : "\x1b[22m";
        }
        if (!styleValid || curStyle.dim != cell.style.dim) {
          sgr += cell.style.dim ? "\x1b[2m" : "\x1b[22m";
        }
        if (!styleValid || curStyle.italic != cell.style.italic) {
          sgr += cell.style.italic ? "\x1b[3m" : "\x1b[23m";
        }
        if (!styleValid || curStyle.underline != cell.style.underline) {
          sgr += cell.style.underline ? "\x1b[4m" : "\x1b[24m";
        }
        if (!styleValid || curStyle.strikethrough != cell.style.strikethrough) {
          sgr += cell.style.strikethrough ? "\x1b[9m" : "\x1b[29m";
        }
        if (!styleValid || curStyle.invert != cell.style.invert) {
          sgr += cell.style.invert ? "\x1b[7m" : "\x1b[27m";
        }
      }

      // Foreground color.
      if (!styleValid || curFg != cell.fg) {
        if (cell.fg.type == CellColor::Type::Default) {
          sgr += "\x1b[39m";
        } else if (cell.fg.type == CellColor::Type::Palette256) {
          sgr += "\x1b[38;5;" + std::to_string(cell.fg.index) + "m";
        } else if (cell.fg.type == CellColor::Type::RGB) {
          sgr += "\x1b[38;2;" + std::to_string(cell.fg.r) + ";" +
                 std::to_string(cell.fg.g) + ";" + std::to_string(cell.fg.b) + "m";
        }
      }

      // Background color.
      if (!styleValid || curBg != cell.bg) {
        if (cell.bg.type == CellColor::Type::Default) {
          sgr += "\x1b[49m";
        } else if (cell.bg.type == CellColor::Type::Palette256) {
          sgr += "\x1b[48;5;" + std::to_string(cell.bg.index) + "m";
        } else if (cell.bg.type == CellColor::Type::RGB) {
          sgr += "\x1b[48;2;" + std::to_string(cell.bg.r) + ";" +
                 std::to_string(cell.bg.g) + ";" + std::to_string(cell.bg.b) + "m";
        }
      }

      curFg = cell.fg;
      curBg = cell.bg;
      curStyle = cell.style;
      styleValid = true;
    }

    if (!sgr.empty()) {
      terminal_.rawWrite(sgr);
    }
  };

  auto emitChar = [](Terminal& term, char32_t ch) {
    // Encode codepoint back to UTF-8.
    if (ch < 0x80) {
      char buf[1] = {static_cast<char>(ch)};
      term.rawWrite(std::string(buf, 1));
    } else if (ch < 0x800) {
      char buf[2] = {
        static_cast<char>(0xC0 | (ch >> 6)),
        static_cast<char>(0x80 | (ch & 0x3F))
      };
      term.rawWrite(std::string(buf, 2));
    } else if (ch < 0x10000) {
      char buf[3] = {
        static_cast<char>(0xE0 | (ch >> 12)),
        static_cast<char>(0x80 | ((ch >> 6) & 0x3F)),
        static_cast<char>(0x80 | (ch & 0x3F))
      };
      term.rawWrite(std::string(buf, 3));
    } else {
      char buf[4] = {
        static_cast<char>(0xF0 | (ch >> 18)),
        static_cast<char>(0x80 | ((ch >> 12) & 0x3F)),
        static_cast<char>(0x80 | ((ch >> 6) & 0x3F)),
        static_cast<char>(0x80 | (ch & 0x3F))
      };
      term.rawWrite(std::string(buf, 4));
    }
  };

  for (int r = 0; r < h; ++r) {
    for (int c = 0; c < w; ++c) {
      const Cell& cell = target[r][c];

      // Skip cells that haven't changed.
      if (hasPrev && cell == prevFrame_[r][c]) {
        continue;
      }

      // Move cursor if not already at this position.
      if (curRow != r + 1 || curCol != c + 1) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "\x1b[%d;%dH", r + 1, c + 1);
        terminal_.rawWrite(buf);
      }

      emitSgr(cell);
      emitChar(terminal_, cell.ch);

      curRow = r + 1;
      curCol = c + 2; // cursor advances by 1 column after writing
    }
  }
}

void App::onResize() {
  terminal_.clearScreen();
  prevFrame_.clear(); // Force full redraw.
  prevW_ = 0;
  prevH_ = 0;
  layout_.invalidate();
  state_.markDirtyPublic();
}

void App::connectAndSetup() {
  try {
    state_.setDaemonReady(false);
    // Show immediate feedback — the user sees this on the first frame.
    state_.setLiveMessage("Connecting to daemon...");

    // Connect with retries and exponential backoff (500ms → 1s → 2s → 4s → 8s → 16s).
    bool connected = false;
    int delayMs = 500;
    constexpr int maxDelayMs = 16000;
    constexpr int maxTotalAttempts = 20;
    for (int attempt = 0; attempt < maxTotalAttempts; ++attempt) {
      if (session_.connect()) {
        connected = true;
        break;
      }
      state_.setLiveMessage(
          "Daemon not ready, retrying in " + std::to_string(delayMs / 1000.0) +
          "s... (attempt " + std::to_string(attempt + 1) + "/" +
          std::to_string(maxTotalAttempts) + ")");
      std::this_thread::sleep_for(std::chrono::milliseconds(delayMs));
      delayMs = std::min(delayMs * 2, maxDelayMs);
    }

    if (!connected) {
      state_.setConnectionStatus(ConnectionStatus::Disconnected);
      state_.addItem(std::make_unique<ErrorMessageItem>(
          "Failed to connect to daemon after " + std::to_string(maxTotalAttempts) +
          " attempts. Is firmiusd running?"));
      return;
    }

    state_.setLiveMessage("Connected. Subscribing to events...");
    state_.setConnectionStatus(ConnectionStatus::Connected);

    // Hook the connect-wizard progress events through the EventRouter into
    // our overlay. EventRouter doesn't know about overlays; we just hand it
    // a callback that forwards into App::onConnectProgress.
    eventRouter_.setOnConnectProgress(
        [this](const firmius::daemon::ConnectProgressSnapshot& snap) {
          onConnectProgress(snap);
        });
    eventRouter_.setOnRewindApplied(
        [this](const firmius::daemon::RewindAppliedSnapshot& snap) {
          onRewindApplied(snap);
        });

    // Subscribe to events — init progress messages will arrive as SystemNoticeItems.
    session_.subscribe([this](const firmius::daemon::DaemonEventEnvelope& envelope) {
      eventRouter_.route(envelope);
    });

    // Decide what to load:
    //   --thread-id <id> → open that exact thread.
    //   -c (continueSession) → resume most-recent thread in the active cwd
    //                          (skipping ones currently locked by another
    //                          firmius client).
    // If either of those is requested but cannot be satisfied, fall through
    // to the default UI snapshot path so the user lands somewhere usable.
    bool didOpenThread = false;
    if (!options_.threadId.empty()) {
      if (dispatcher_.openThread(options_.threadId)) {
        didOpenThread = true;
      } else {
        notifications_.error("Could not open thread '" + options_.threadId +
                             "'. Starting on the welcome screen.");
      }
    } else if (options_.continueSession) {
      try {
        const std::string resumeCwd =
            options_.cwd.empty()
                ? std::filesystem::current_path().string()
                : options_.cwd;
        auto threads = session_.listThreadOverviews(resumeCwd);
        // Drop ones owned by another live client; opening them would
        // either be denied by the daemon or stomp on a peer.
        threads.erase(
            std::remove_if(threads.begin(), threads.end(),
                           [](const firmius::daemon::ThreadOverview &o) {
                             return o.lockedByOtherClient;
                           }),
            threads.end());
        if (!threads.empty()) {
          auto newest = std::max_element(
              threads.begin(), threads.end(),
              [](const firmius::daemon::ThreadOverview &a,
                 const firmius::daemon::ThreadOverview &b) {
                return a.thread.lastActiveAt < b.thread.lastActiveAt;
              });
          if (dispatcher_.openThread(newest->thread.threadId)) {
            didOpenThread = true;
          }
        }
        if (!didOpenThread) {
          notifications_.info(
              "No previous session found in " + resumeCwd +
              ". Starting fresh.");
        }
      } catch (const std::exception &e) {
        notifications_.error(std::string("Could not resume last session: ") +
                             e.what());
      }
    }

    if (!didOpenThread) {
      state_.setLiveMessage("Loading UI snapshot...");
      firmius::daemon::UiSnapshotRequest request;
      // uiSnapshot now blocks until daemon init is complete (up to 30s).
      firmius::daemon::UiSnapshot uiSnap = session_.client().uiSnapshot(request);
      std::string label = uiSnap.config.config.defaultProviderId + "/" +
                          uiSnap.config.config.defaultModelId;
      state_.setModelLabel(label);
      state_.setAgentPurpose(uiSnap.config.config.defaultLeadPersona);
      if (uiSnap.focusedAgent.has_value()) {
        state_.setAgentContextUsage(ContextUsage{
            uiSnap.focusedAgent->contextWindowTokens,
            uiSnap.focusedAgent->contextUsedTokens,
            uiSnap.focusedAgent->contextSentTokens,
        });
        if (uiSnap.focusedAgent->contextWindowTokens > 0) {
          const uint32_t displayTokens = uiSnap.focusedAgent->contextUsedTokens > 0
                                             ? uiSnap.focusedAgent->contextUsedTokens
                                             : uiSnap.focusedAgent->contextSentTokens;
          state_.setAgentContextWindow(
              std::to_string(displayTokens / 1000) + "k/" +
              std::to_string(uiSnap.focusedAgent->contextWindowTokens / 1000) +
              "k");
        }
      } else {
        for (const auto& info : uiSnap.models.models) {
          if (info.provider == uiSnap.config.config.defaultProviderId &&
              info.id == uiSnap.config.config.defaultModelId) {
            state_.setAgentContextUsage(ContextUsage{info.contextWindow, 0, 0});
            state_.setAgentContextWindow("0/" +
                                         std::to_string(info.contextWindow / 1000) +
                                         "k");
            break;
          }
        }
      }
      if (uiSnap.focusedAgentTodo.has_value() && uiSnap.focusedAgent.has_value()) {
        state_.setAgentTodos(uiSnap.focusedAgent->agentId,
                             uiSnap.focusedAgentTodo->items);
      }
      state_.resetWelcomeState();
      state_.setHookState(uiSnap.hooks);
    }

    // Welcome message.
    auto threadId = state_.threadId();
    std::string welcomeText = threadId.empty()
        ? "Ready. Type a message or use /new to start."
        : "Ready. Thread: " + threadId;
    state_.setLiveMessage("");
    state_.setDaemonReady(true);
    state_.addItem(std::make_unique<SystemNoticeItem>(welcomeText));

  } catch (const std::exception& e) {
    state_.setDaemonReady(false);
    state_.setConnectionStatus(ConnectionStatus::Disconnected);
    state_.addItem(std::make_unique<ErrorMessageItem>(e.what()));
  }
}

void App::openModelsMenu() {
  try {
    auto models = session_.client().listModels(true);
    std::vector<MenuList::Item> items;
    std::unordered_map<std::string, uint32_t> contextWindows;
    const std::string currentModel = state_.modelLabel();
    for (const auto& m : models.models) {
      MenuList::Item item;
      item.label = m.id;
      item.detail = m.provider;
      if (m.contextWindow > 0) {
        item.detail += " · " + humanizeTokenWindow(m.contextWindow) + " ctx";
      }
      item.id = m.provider + ":" + m.id;
      item.marked = (currentModel == m.provider + "/" + m.id);
      contextWindows[item.id] = m.contextWindow;
      items.push_back(std::move(item));
    }
    menuList_.open();
    menuList_.setTitle("Select Model");
    menuList_.setItems(std::move(items));
    menuList_.setOnSelect([this, contextWindows = std::move(contextWindows)](const MenuList::Item& item) {
      auto sep = item.id.find(':');
      if (sep != std::string::npos) {
        auto providerId = item.id.substr(0, sep);
        auto modelId = item.id.substr(sep + 1);
        uint32_t contextWindowTokens = 0;
        auto it = contextWindows.find(item.id);
        if (it != contextWindows.end()) {
          contextWindowTokens = it->second;
        }
        applyModelSelection(providerId, modelId, contextWindowTokens);
      }
      dismissOverlay();
    });
    menuList_.setOnDismiss([this]() { dismissOverlay(); });
    activeOverlay_ = &menuList_;
    state_.markDirtyPublic();
  } catch (const std::exception& e) {
    notifications_.error(std::string("Failed to list models: ") + e.what());
  }
}

void App::openResumeMenu() {
  try {
    const std::string currentCwd =
        std::filesystem::current_path().string();
    auto threads = session_.listThreadOverviews(currentCwd);
    std::vector<MenuList::Item> items;
    for (const auto& t : threads) {
      MenuList::Item item;
      item.label = t.thread.title.empty() ? t.thread.threadId : t.thread.title;
      item.detail = formatRelativeThreadTime(t.thread.lastActiveAt) + " · " +
                    threadCountsDetail(t.agentCount, t.artifactCount);
      if (t.lockedByOtherClient) {
        item.detail += " · " + lockDetail(t);
      }
      item.detail += " · " +
                     t.thread.threadId.substr(
                         0, std::min<std::size_t>(8, t.thread.threadId.size())) +
                     "...";
      item.id = t.thread.threadId;
      items.push_back(std::move(item));
    }
    menuList_.open();
    menuList_.setTitle("Resume Session");
    menuList_.setItems(std::move(items));
    menuList_.setOnSelect([this](const MenuList::Item& item) {
      state_.setLiveMessage("Loading thread...");
      state_.markDirtyPublic();
      renderFrame();
      dispatcher_.openThread(item.id);
      dismissOverlay();
    });
    menuList_.setOnDismiss([this]() { dismissOverlay(); });
    activeOverlay_ = &menuList_;
    state_.markDirtyPublic();
  } catch (const std::exception& e) {
    notifications_.error(std::string("Failed to list threads: ") + e.what());
  }
}

void App::openAccountsOverlay(const std::string& providerId) {
  try {
    firmius::daemon::AccountsRequest req;
    req.providerId = providerId;
    auto accounts = session_.client().listAccounts(req);

    firmius::daemon::QuotaSnapshot quotas;
    try {
      firmius::daemon::QuotasRequest qreq;
      qreq.providerId = providerId;
      quotas = session_.client().getCachedQuotas(qreq);
      if (quotas.buckets.empty()) {
        quotas = session_.client().getQuotas(qreq);
      }
    } catch (...) {}

    auto [termW, termH] = terminal_.size();
    (void)termH;

    accountsOverlay_.load(providerId, std::move(accounts),
                          std::move(quotas), termW);
    accountsOverlay_.setOnDismiss([this]() { dismissOverlay(); });
    accountsOverlay_.open();
    activeOverlay_ = &accountsOverlay_;
    state_.markDirtyPublic();
  } catch (const std::exception& e) {
    notifications_.error(std::string("Failed to fetch accounts: ") + e.what());
  }
}

// ── /connect wizard wiring ───────────────────────────────────────────────
//
// Flow:
//   /connect <provider>
//      → openConnectOverlay → beginConnectFlow(addAdditional=false)
//        - if existingAccounts → SystemNoticeItem (so the user re-runs with
//          intent rather than us silently second-guessing them)
//        - else open ConnectOverlay with the first prompt
//   user submits answer
//      → ConnectOverlay::onSubmit_ → submitConnect RPC
//        - if next prompt → loadPrompt
//        - if readyToFinalize → finalizeConnect (async on the daemon)
//   ConnectProgress events arrive on the subscription stream, EventRouter
//   forwards them to onConnectProgress which calls overlay.updateProgress.

void App::openConnectOverlay(const std::string& providerId) {
  if (providerId.empty()) return;
  // Always start with addAdditional=false; if accounts exist we add a
  // system notice and let the user opt in.
  beginConnectFlow(providerId, /*addAdditional=*/false);
}

void App::beginConnectFlow(const std::string& providerId,
                            bool addAdditional) {
  firmius::daemon::ConnectBeginResponse resp;
  try {
    resp = session_.beginConnect(providerId, addAdditional);
  } catch (const std::exception& e) {
    notifications_.error(std::string("/connect: ") + e.what());
    return;
  }

  if (!resp.errorMessage.empty()) {
    state_.addItem(std::make_unique<SystemNoticeItem>(
        "/connect: " + resp.errorMessage));
    return;
  }

  if (resp.existingAccounts) {
    // v1 used a confirm modal; v2 keeps it lighter — drop a system notice
    // pointing the user at the explicit re-run. Same effect, less screen
    // furniture, and reuses the existing scrollback as the audit trail.
    state_.addItem(std::make_unique<SystemNoticeItem>(
        "An account already exists for " + providerId +
        ". Re-run /connect " + providerId +
        " (it will add a new account this time)."));
    // Auto-confirm by re-calling with addAdditional=true. If you prefer the
    // explicit confirm, comment this out — but the system notice already
    // advertised the re-run, so the user has signal either way.
    auto retry = session_.beginConnect(providerId, /*addAdditional=*/true);
    if (!retry.errorMessage.empty()) {
      state_.addItem(std::make_unique<SystemNoticeItem>(
          "/connect: " + retry.errorMessage));
      return;
    }
    resp = std::move(retry);
  }

  // Configure the overlay.
  connectOverlay_.setProviderId(resp.providerId.empty() ? providerId
                                                       : resp.providerId);
  connectOverlay_.setProviderKind(resp.providerKind);
  connectOverlay_.setSessionId(resp.sessionId);

  connectOverlay_.setOnSubmit([this](const std::string& answer) {
    const auto sid = connectOverlay_.sessionId();
    if (sid.empty()) return;
    firmius::daemon::ConnectSubmitResponse sresp;
    try {
      sresp = session_.submitConnect(sid, answer);
    } catch (const std::exception& e) {
      connectOverlay_.showError(std::string("submit failed: ") + e.what());
      state_.markDirtyPublic();
      return;
    }
    if (!sresp.errorMessage.empty()) {
      connectOverlay_.showError(sresp.errorMessage);
      state_.markDirtyPublic();
      return;
    }
    if (sresp.prompt.has_value()) {
      connectOverlay_.loadPrompt(*sresp.prompt);
    } else if (sresp.readyToFinalize) {
      connectOverlay_.markReadyToFinalize();
      // Kick off finalize immediately; outcome arrives via event stream.
      try {
        auto fresp = session_.finalizeConnect(sid);
        if (!fresp.accepted && !fresp.errorMessage.empty()) {
          firmius::daemon::ConnectProgressSnapshot snap;
          snap.sessionId = sid;
          snap.phase = firmius::daemon::ConnectProgressPhase::Failed;
          snap.message = fresp.errorMessage;
          connectOverlay_.updateProgress(std::move(snap));
        }
      } catch (const std::exception& e) {
        firmius::daemon::ConnectProgressSnapshot snap;
        snap.sessionId = sid;
        snap.phase = firmius::daemon::ConnectProgressPhase::Failed;
        snap.message = std::string("finalize failed: ") + e.what();
        connectOverlay_.updateProgress(std::move(snap));
      }
    }
    state_.markDirtyPublic();
  });

  connectOverlay_.setOnCancel([this]() {
    const auto sid = connectOverlay_.sessionId();
    if (!sid.empty()) {
      try { session_.cancelConnect(sid); } catch (...) {}
    }
    dismissOverlay();
  });

  connectOverlay_.setOnDismiss([this]() {
    dismissOverlay();
  });

  // Initial state.
  if (resp.prompt.has_value()) {
    connectOverlay_.loadPrompt(*resp.prompt);
  } else if (resp.readyToFinalize && !resp.sessionId.empty()) {
    connectOverlay_.markReadyToFinalize();
    try { session_.finalizeConnect(resp.sessionId); } catch (...) {}
  }

  connectOverlay_.open();
  activeOverlay_ = &connectOverlay_;
  state_.markDirtyPublic();
}

void App::onConnectProgress(
    const firmius::daemon::ConnectProgressSnapshot& snap) {
  if (activeOverlay_ != &connectOverlay_) return;
  if (!snap.sessionId.empty() &&
      snap.sessionId != connectOverlay_.sessionId()) {
    // Stale event from a previous wizard.
    return;
  }
  connectOverlay_.updateProgress(snap);
  // Surface the success/failure outcome in the chat scrollback too, so it
  // stays visible after the overlay is dismissed.
  using P = firmius::daemon::ConnectProgressPhase;
  if (snap.phase == P::Succeeded) {
    state_.addItem(std::make_unique<SystemNoticeItem>(
        "/connect: " + (snap.message.empty() ? std::string("connected.")
                                              : snap.message)));
  } else if (snap.phase == P::Failed) {
    state_.addItem(std::make_unique<SystemNoticeItem>(
        "/connect failed: " + (snap.message.empty()
                                   ? std::string("(no detail)")
                                   : snap.message)));
  }
  state_.markDirtyPublic();
}

// ── /undo Rewind wiring ──────────────────────────────────────────────────
//
// Flow:
//   /undo → openRewindOverlay()
//      - Pulls the current agent's transcript from the daemon.
//      - Filters to user-message turns (those are the candidates).
//      - Builds RewindOverlay::TurnEntry list, hands it to the overlay.
//   User navigates → onRewindPreviewRequest fires rewind.preview.
//   User confirms a mode → onRewindExecuteRequest fires rewind.execute.
//      - On success the daemon emits a RewindApplied event on the
//        subscription stream, which lands in onRewindApplied.
//   onRewindApplied closes the overlay, refreshes the transcript so the
//      undone turns disappear from the UI, and surfaces a SystemNoticeItem
//      so the user has a record of what just happened.

namespace {

std::string previewForUserTurn(const firmius::shared::AgentTurn& turn) {
  for (const auto &msg : turn.messages) {
    if (msg.role != firmius::shared::Role::User) continue;
    for (const auto &part : msg.content) {
      if (std::holds_alternative<firmius::shared::TextContent>(part)) {
        const auto &text = std::get<firmius::shared::TextContent>(part).text;
        auto eol = text.find('\n');
        std::string head = eol == std::string::npos ? text : text.substr(0, eol);
        // Strip whitespace.
        while (!head.empty() &&
               std::isspace(static_cast<unsigned char>(head.front()))) {
          head.erase(head.begin());
        }
        while (!head.empty() &&
               std::isspace(static_cast<unsigned char>(head.back()))) {
          head.pop_back();
        }
        if (!head.empty()) return head;
      }
    }
  }
  return {};
}

}  // namespace

void App::openRewindOverlay() {
  const std::string threadId = state_.threadId();
  const std::string agentId  = state_.focusedAgentId();
  if (threadId.empty() || agentId.empty()) {
    notifications_.warning("/undo: no active thread to rewind.");
    return;
  }

  std::optional<firmius::daemon::TranscriptSnapshot> snapshot;
  try {
    snapshot = session_.getTranscript(threadId, agentId);
  } catch (const std::exception& e) {
    notifications_.error(std::string("/undo: ") + e.what());
    return;
  }
  if (!snapshot.has_value() || snapshot->rawTurns.empty()) {
    notifications_.warning("/undo: nothing to rewind in this thread yet.");
    return;
  }

  // Build the turn list — user messages only, newest first.
  std::vector<RewindOverlay::TurnEntry> entries;
  for (auto it = snapshot->rawTurns.rbegin(); it != snapshot->rawTurns.rend(); ++it) {
    bool isUser = false;
    std::uint64_t ts = 0;
    for (const auto &msg : it->messages) {
      if (msg.role == firmius::shared::Role::User) {
        isUser = true;
        ts = msg.timestamp;
        break;
      }
    }
    if (!isUser) continue;
    RewindOverlay::TurnEntry entry;
    entry.turnId = it->turnId;
    entry.preview = previewForUserTurn(*it);
    if (entry.preview.empty()) entry.preview = "(no text)";
    entry.createdAtMs = ts;
    entries.push_back(std::move(entry));
  }
  if (entries.empty()) {
    notifications_.warning("/undo: no user messages to rewind to.");
    return;
  }

  rewindOverlay_.setOnPreviewRequest([this](const std::string& turnId) {
    onRewindPreviewRequest(turnId);
  });
  rewindOverlay_.setOnExecute(
      [this](const std::string& turnId, firmius::daemon::RewindMode mode) {
        onRewindExecuteRequest(turnId, mode);
      });
  rewindOverlay_.setOnDismiss([this]() { dismissOverlay(); });

  rewindOverlay_.open();
  rewindOverlay_.setEntries(std::move(entries));  // also kicks first preview
  activeOverlay_ = &rewindOverlay_;
  state_.markDirtyPublic();
}

void App::onRewindPreviewRequest(const std::string& targetTurnId) {
  const std::string threadId = state_.threadId();
  const std::string agentId  = state_.focusedAgentId();
  if (threadId.empty() || agentId.empty() || targetTurnId.empty()) return;
  try {
    auto preview = session_.previewRewind(threadId, agentId, targetTurnId);
    rewindOverlay_.setPreview(std::move(preview));
    state_.markDirtyPublic();
  } catch (const std::exception& e) {
    rewindOverlay_.showError(std::string("preview failed: ") + e.what());
    state_.markDirtyPublic();
  }
}

void App::onRewindExecuteRequest(const std::string& targetTurnId,
                                  firmius::daemon::RewindMode mode) {
  const std::string threadId = state_.threadId();
  const std::string agentId  = state_.focusedAgentId();
  if (threadId.empty() || agentId.empty() || targetTurnId.empty()) return;
  rewindOverlay_.setExecuting(true);
  state_.markDirtyPublic();
  try {
    auto resp = session_.executeRewind(threadId, agentId, targetTurnId, mode);
    if (!resp.applied) {
      rewindOverlay_.setExecuting(false);
      rewindOverlay_.showError(resp.errorMessage.empty()
                                    ? std::string("rewind failed")
                                    : resp.errorMessage);
      state_.markDirtyPublic();
      return;
    }
    // Success: the RewindApplied event will arrive on the subscription
    // stream and trigger onRewindApplied which closes the overlay and
    // refreshes the transcript.
  } catch (const std::exception& e) {
    rewindOverlay_.setExecuting(false);
    rewindOverlay_.showError(std::string("rewind failed: ") + e.what());
    state_.markDirtyPublic();
  }
}

void App::openRedoOverlay() {
  // Pre-fetch the recent transcript-undo actions list synchronously. This
  // happens on the main thread (command execution) so a blocking RPC is
  // safe — same pattern as openAccountsOverlay.
  const std::string threadId = state_.threadId();
  const std::string agentId  = state_.focusedAgentId();
  if (threadId.empty() || agentId.empty()) {
    notifications_.warning("/redo: no active agent.");
    return;
  }
  firmius::daemon::RedoPreviewResponse preview;
  try {
    preview = session_.previewRedo(threadId, agentId, /*limit=*/10);
  } catch (const std::exception &e) {
    notifications_.error(std::string("/redo: failed to load history: ") +
                          e.what());
    return;
  }
  if (!preview.errorMessage.empty()) {
    notifications_.warning("/redo: " + preview.errorMessage);
    return;
  }
  if (preview.actions.empty()) {
    notifications_.info("/redo: no undo actions available to redo.");
    return;
  }

  redoOverlay_.setOnExecute(
      [this](const std::string& undoActionId,
             firmius::daemon::RedoMode mode) {
        onRedoExecuteRequest(undoActionId, mode);
      });
  redoOverlay_.setOnDismiss([this]() { dismissOverlay(); });

  redoOverlay_.open();
  redoOverlay_.setActions(std::move(preview.actions));
  activeOverlay_ = &redoOverlay_;
  state_.markDirtyPublic();
}

void App::onRedoExecuteRequest(const std::string& undoActionId,
                                firmius::daemon::RedoMode mode) {
  const std::string threadId = state_.threadId();
  const std::string agentId  = state_.focusedAgentId();
  if (threadId.empty() || agentId.empty() || undoActionId.empty()) return;
  redoOverlay_.setExecuting(true);
  state_.markDirtyPublic();
  try {
    auto resp = session_.executeRedo(threadId, agentId, undoActionId, mode);
    if (!resp.applied) {
      redoOverlay_.setExecuting(false);
      redoOverlay_.showError(resp.errorMessage.empty()
                                  ? std::string("redo failed")
                                  : resp.errorMessage);
      state_.markDirtyPublic();
      return;
    }
    // Success: the daemon broadcasts RewindApplied with negative
    // turnsUndone, which onRewindApplied recognizes as a redo and
    // closes the overlay + reloads the transcript.
  } catch (const std::exception& e) {
    redoOverlay_.setExecuting(false);
    redoOverlay_.showError(std::string("redo failed: ") + e.what());
    state_.markDirtyPublic();
  }
}

void App::openRouterOverlay() {
  try {
    auto routerSnap = session_.client().getRouterConfig();
    auto modelSnap = session_.client().listModels(true);
    routerOverlay_.seed(std::move(routerSnap), std::move(modelSnap.models));
  } catch (const std::exception& e) {
    notifications_.error(std::string("/router: ") + e.what());
    return;
  }
  routerOverlay_.setOnSave([this](const firmius::daemon::RouterConfigUpdateRequest& req) {
    try {
      session_.client().updateRouterConfig(req);
    } catch (const std::exception& e) {
      notifications_.error(std::string("Router save failed: ") + e.what());
    }
  });
  routerOverlay_.setOnDismiss([this]() { dismissOverlay(); });
  routerOverlay_.open();
  activeOverlay_ = &routerOverlay_;
  state_.markDirtyPublic();
}

void App::openPurposesOverlay() {
  try {
    auto purposesSnap = session_.client().getPurposesConfig();
    auto routerSnap = session_.client().getRouterConfig();
    auto personaSnap = session_.client().listPersonas();

    std::vector<std::string> purposes;
    for (const auto& p : personaSnap.personas) purposes.push_back(p.name);
    for (const auto& [k, _] : purposesSnap.purposeRoutes) purposes.push_back(k);
    std::sort(purposes.begin(), purposes.end());
    purposes.erase(std::unique(purposes.begin(), purposes.end()), purposes.end());

    std::vector<std::string> categories;
    for (const auto& [name, _] : routerSnap.categories) categories.push_back(name);
    std::sort(categories.begin(), categories.end());

    purposesOverlay_.seed(std::move(purposes), std::move(purposesSnap.purposeRoutes),
                          std::move(categories));
  } catch (const std::exception& e) {
    notifications_.error(std::string("/purposes: ") + e.what());
    return;
  }
  purposesOverlay_.setOnSave([this](const firmius::daemon::PurposesConfigUpdateRequest& req) {
    try {
      session_.client().updatePurposesConfig(req);
    } catch (const std::exception& e) {
      notifications_.error(std::string("Purposes save failed: ") + e.what());
    }
  });
  purposesOverlay_.setOnDismiss([this]() { dismissOverlay(); });
  purposesOverlay_.open();
  activeOverlay_ = &purposesOverlay_;
  state_.markDirtyPublic();
}

void App::openPermissionPromptOverlay() {
  auto perm = state_.pendingPermission();
  if (!perm) return;
  // Compute queue context for the [i/N] badge + next-up hint. The
  // overlay always opens on the front of the queue, so queueIndex is
  // 1-based and queueSize is the full pending count at this moment.
  auto queue = state_.pendingPermissions();
  int queueIndex = 1;
  int queueSize = static_cast<int>(queue.size());
  std::string nextHint;
  if (queue.size() >= 2) {
    const auto &n = queue[1];
    if (!n.command.empty())            nextHint = n.command;
    else if (!n.targetPath.empty())    nextHint = n.targetPath;
    else if (!n.url.empty())           nextHint = n.url;
    else if (!n.persona.empty())       nextHint = "spawn " + n.persona;
    else                                nextHint = n.title;
  }
  permissionOverlay_.setPermission(*perm, queueIndex, queueSize,
                                    std::move(nextHint));
  permissionOverlay_.setOnAllowOnce([this](const std::string& id) {
    dispatcher_.resolvePermission(id,
                                   firmius::shared::PermissionResponse::AllowOnce);
    dismissOverlay();
    // If more permissions are queued, open the next one.
    if (state_.hasPendingPermissions()) openPermissionPromptOverlay();
  });
  permissionOverlay_.setOnDeny([this](const std::string& id) {
    dispatcher_.resolvePermission(id, firmius::shared::PermissionResponse::Deny);
    dismissOverlay();
    if (state_.hasPendingPermissions()) openPermissionPromptOverlay();
  });
  permissionOverlay_.setOnAllowAlways(
      [this](const std::string& id, const std::vector<std::string>& picks) {
        if (picks.empty()) {
          // No tailored rules — fall back to "allow tool for session".
          dispatcher_.resolvePermission(
              id, firmius::shared::PermissionResponse::AllowAllToolSession);
        } else {
          dispatcher_.resolvePermissionWithRules(id, picks);
        }
        dismissOverlay();
        if (state_.hasPendingPermissions()) openPermissionPromptOverlay();
      });
  permissionOverlay_.setOnDismiss([this]() {
    // Esc on the overlay = deny (safer default than swallowing the request).
    auto p = state_.pendingPermission();
    if (p) {
      dispatcher_.resolvePermission(p->requestId,
                                     firmius::shared::PermissionResponse::Deny);
    }
    dismissOverlay();
  });
  permissionOverlay_.open();
  activeOverlay_ = &permissionOverlay_;
  state_.markDirtyPublic();
}

void App::openPermissionsOverlay() {
  firmius::daemon::PermissionListRulesResponse rulesSnapshot;
  firmius::daemon::PermissionQueueSnapshot modeSnapshot;
  try {
    rulesSnapshot = session_.client().listPolicyRules();
    modeSnapshot = session_.client().getPermissionMode(
        firmius::daemon::PermissionModeRequest{});
  } catch (const std::exception &e) {
    notifications_.error(std::string("/permissions: ") + e.what());
    return;
  }
  permissionsOverlay_.seedRules(std::move(rulesSnapshot));
  permissionsOverlay_.seedModes(modeSnapshot.modes, modeSnapshot.activeModeId);

  permissionsOverlay_.setOnDeleteRule([this](const std::string& id) {
    try {
      auto resp = session_.client().deletePolicyRule(
          firmius::daemon::PermissionDeleteRuleRequest{id});
      if (!resp.removed && !resp.errorMessage.empty()) {
        notifications_.error("Delete rule: " + resp.errorMessage);
      }
      return resp.removed;
    } catch (const std::exception &e) {
      notifications_.error(std::string("Delete rule: ") + e.what());
      return false;
    }
  });
  permissionsOverlay_.setOnSetActiveMode([this](const std::string& id) {
    try {
      firmius::daemon::PermissionModeUpdateRequest req;
      req.modeId = id;
      auto snap = session_.client().setPermissionMode(req);
      state_.setActiveModeId(snap.activeModeId);
      return true;
    } catch (const std::exception &e) {
      notifications_.error(std::string("Set mode: ") + e.what());
      return false;
    }
  });
  permissionsOverlay_.setOnCreateMode(
      [this](const std::string& name, bool seedFromActive) -> std::string {
    try {
      firmius::daemon::PermissionCreateModeRequest req;
      req.name = name;
      req.seedFromActive = seedFromActive;
      auto resp = session_.client().createPermissionMode(req);
      if (resp.modeId.empty() && !resp.errorMessage.empty()) {
        notifications_.error("Create mode: " + resp.errorMessage);
        return "";
      }
      // Refresh modes list.
      auto snap = session_.client().getPermissionMode(
          firmius::daemon::PermissionModeRequest{});
      permissionsOverlay_.seedModes(snap.modes, snap.activeModeId);
      std::vector<AppState::ModeSummary> ms;
      for (const auto &m : snap.modes) {
        ms.push_back({m.id, m.name, m.description, m.builtIn});
      }
      state_.setModes(std::move(ms));
      state_.setActiveModeId(snap.activeModeId);
      return resp.modeId;
    } catch (const std::exception &e) {
      notifications_.error(std::string("Create mode: ") + e.what());
      return "";
    }
  });
  permissionsOverlay_.setOnRenameMode(
      [this](const std::string& id, const std::string& newName) {
    try {
      firmius::daemon::PermissionRenameModeRequest req;
      req.modeId = id;
      req.newName = newName;
      auto resp = session_.client().renamePermissionMode(req);
      if (!resp.ok && !resp.errorMessage.empty()) {
        notifications_.error("Rename mode: " + resp.errorMessage);
      }
      auto snap = session_.client().getPermissionMode(
          firmius::daemon::PermissionModeRequest{});
      permissionsOverlay_.seedModes(snap.modes, snap.activeModeId);
      std::vector<AppState::ModeSummary> ms;
      for (const auto &m : snap.modes) {
        ms.push_back({m.id, m.name, m.description, m.builtIn});
      }
      state_.setModes(std::move(ms));
      return resp.ok;
    } catch (const std::exception &e) {
      notifications_.error(std::string("Rename mode: ") + e.what());
      return false;
    }
  });
  permissionsOverlay_.setOnDeleteMode([this](const std::string& id) {
    try {
      firmius::daemon::PermissionDeleteModeRequest req;
      req.modeId = id;
      auto resp = session_.client().deletePermissionMode(req);
      if (!resp.removed && !resp.errorMessage.empty()) {
        notifications_.error("Delete mode: " + resp.errorMessage);
      }
      auto snap = session_.client().getPermissionMode(
          firmius::daemon::PermissionModeRequest{});
      permissionsOverlay_.seedModes(snap.modes, snap.activeModeId);
      std::vector<AppState::ModeSummary> ms;
      for (const auto &m : snap.modes) {
        ms.push_back({m.id, m.name, m.description, m.builtIn});
      }
      state_.setModes(std::move(ms));
      return resp.removed;
    } catch (const std::exception &e) {
      notifications_.error(std::string("Delete mode: ") + e.what());
      return false;
    }
  });
  permissionsOverlay_.setOnReload([this]() {
    try {
      auto resp = session_.client().reloadPolicy();
      if (!resp.ok) {
        notifications_.error("Reload policy: " + resp.errorMessage);
        return false;
      }
      permissionsOverlay_.seedRules(session_.client().listPolicyRules());
      auto snap = session_.client().getPermissionMode(
          firmius::daemon::PermissionModeRequest{});
      permissionsOverlay_.seedModes(snap.modes, snap.activeModeId);
      return true;
    } catch (const std::exception &e) {
      notifications_.error(std::string("Reload policy: ") + e.what());
      return false;
    }
  });
  permissionsOverlay_.setOnDismiss([this]() { dismissOverlay(); });
  permissionsOverlay_.open();
  activeOverlay_ = &permissionsOverlay_;
  state_.markDirtyPublic();
}

void App::onRewindApplied(const firmius::daemon::RewindAppliedSnapshot& snap) {
  // CAUTION: this runs on the JsonRpcTransport reader thread (event
  // listeners are invoked synchronously from inside handleMessage).
  // We must NOT call session_/dispatcher_ RPCs here — that would deadlock
  // the reader: sendRequest blocks waiting on a future that only this
  // same reader thread can complete.
  //
  // Defer the transcript reload to the main loop instead. The overlay
  // close + scrollback notice are mutex-safe and don't issue RPCs, so
  // they're fine to do inline.
  if (activeOverlay_ == &rewindOverlay_) {
    dismissOverlay();
  }
  if (activeOverlay_ == &redoOverlay_) {
    dismissOverlay();
  }
  // The daemon repurposes RewindApplied for forward redo broadcasts —
  // negative turnsUndone signals "this was a redo, not an undo". Pick a
  // user-readable verb based on the sign.
  const bool isRedo = snap.turnsUndone < 0;
  const int turns = std::abs(snap.turnsUndone);
  const int batches = std::abs(snap.editBatchesUndone);
  std::string msg = isRedo
                        ? std::string("Redid ") + std::to_string(turns) +
                              (turns == 1 ? " turn" : " turns")
                        : std::string("Rewound ") + std::to_string(turns) +
                              (turns == 1 ? " turn" : " turns");
  if (batches > 0) {
    msg += isRedo
               ? "; reapplied " + std::to_string(batches) +
                     (batches == 1 ? " edit batch" : " edit batches")
               : "; restored " + std::to_string(batches) +
                     (batches == 1 ? " edit batch" : " edit batches");
  }
  msg += ".";
  state_.addItem(std::make_unique<SystemNoticeItem>(std::move(msg)));
  state_.markDirtyPublic();

  // Queue the transcript reload for the main loop.
  const std::string agentId = state_.focusedAgentId();
  postDeferred([this, agentId]() {
    dispatcher_.loadTranscriptForAgent(agentId, true);
  });
}

void App::postDeferred(std::function<void()> action) {
  std::lock_guard<std::mutex> lock(deferredMutex_);
  deferredActions_.push_back(std::move(action));
}

void App::drainDeferredActions() {
  // Snapshot under lock, run outside. Actions may post more actions
  // (legitimate use case); those fire on the next tick.
  std::vector<std::function<void()>> snapshot;
  {
    std::lock_guard<std::mutex> lock(deferredMutex_);
    snapshot.swap(deferredActions_);
  }
  for (auto &action : snapshot) {
    try {
      action();
    } catch (const std::exception &e) {
      notifications_.error(std::string("deferred action failed: ") + e.what());
    } catch (...) {
      notifications_.error("deferred action failed: unknown error");
    }
  }
}

void App::applyTheme(const std::string& name) {
  if (!ThemeManager::instance().setTheme(name)) {
    // Wrong-theme-name is the user's mistake, not a system event — surface
    // as a fading warning toast rather than poisoning the transcript.
    notifications_.warning("Unknown theme: " + name, "theme");
    return;
  }
  prevFrame_.clear();
  prevW_ = 0;
  prevH_ = 0;
  state_.clearScrollback();
  lastSyncedItemCount_ = 0;
  lastSyncedRowCounts_.clear();
  state_.markDirtyPublic();
  notifications_.success("Theme: " + name, "theme");
}

void App::dismissMenu() {
  dismissOverlay();
}

void App::dismissOverlay() {
  if (activeOverlay_) {
    activeOverlay_->close();
    activeOverlay_ = nullptr;
    state_.markDirtyPublic();
  }
  dismissAutocomplete();
}

// ── Bracketed paste commit ────────────────────────────────────────────────
//
// Called when the terminal sends `\x1b[201~` (end of bracketed paste). We
// have the full pasted bytes in `pasteBuffer_`. Heuristic from v1: a
// single-line paste over 1KB looks like base64 image data; treat it as an
// image. Multi-line text gets stored as a Pasted text block (collapsed
// placeholder in the buffer + real content on the side). Single-line
// short text gets inserted directly so it's editable.
void App::commitPasteBuffer() {
  if (pasteBuffer_.empty()) return;
  std::string buf = std::move(pasteBuffer_);
  pasteBuffer_.clear();

  // Strip a single trailing newline — terminals sometimes append one.
  if (!buf.empty() && buf.back() == '\n') buf.pop_back();
  // Normalise CR/CRLF inside the paste to LF so multi-line text stays
  // multi-line.
  std::string normalised;
  normalised.reserve(buf.size());
  for (size_t i = 0; i < buf.size(); ++i) {
    if (buf[i] == '\r') {
      normalised += '\n';
      if (i + 1 < buf.size() && buf[i + 1] == '\n') ++i;
    } else {
      normalised += buf[i];
    }
  }

  int lineCount = 1;
  for (char c : normalised) if (c == '\n') ++lineCount;

  // Heuristic: a single very long line that looks like base64 → image.
  // Real image paste comes through Ctrl+V via a different code path
  // (see App::pasteImageFromClipboard); this is the fallback for
  // terminals that splat the raw clipboard payload through bracketed
  // paste.
  const bool likelyImage = (lineCount == 1 && normalised.size() > 1024);
  if (likelyImage) {
    state_.insertPastedImage(std::move(normalised), "image/png");
    state_.markDirtyPublic();
    return;
  }

  // Multi-line text → a collapsible Pasted block. Single-line stays
  // inline so the user can keep editing.
  if (lineCount >= 2) {
    state_.insertPastedText(std::move(normalised));
    state_.markDirtyPublic();
    return;
  }

  state_.insertAtCursor(normalised);
  if (autocomplete_.active) {
    autocomplete_.selectedIndex = 0;
    autocomplete_.scrollOffset = 0;
    updateAutocomplete();
  }
}

void App::submitInputBuffer() {
  if (!state_.daemonReady()) {
    return;
  }
  auto text = state_.inputBuffer();
  // Drain pasted blocks. Text blocks get their placeholders substituted
  // with the real content in the outgoing message; image blocks are
  // separated out into the ImageContent vector and their placeholders
  // are dropped from the text (the assistant gets the image attachment
  // directly, not a "[Pasted #...]" marker).
  auto blocks = state_.takePastedBlocks();
  // Legacy pendingPastedImages_ — retained for older code paths that
  // might still push to it; gets merged with the new block-based ones.
  auto legacyImages = std::move(pendingPastedImages_);
  pendingPastedImages_.clear();

  std::vector<firmius::shared::ImageContent> images = std::move(legacyImages);
  for (auto &blk : blocks) {
    using K = AppState::PastedBlockKind;
    if (blk.kind == K::Text) {
      // Replace the first occurrence of that block's placeholder with
      // its full content. We rebuild the placeholder string here so we
      // don't need a second public API on AppState.
      const std::string placeholder =
          "[Pasted #" + std::to_string(blk.id) + ": " +
          std::to_string(blk.lineCount) +
          (blk.lineCount == 1 ? " line]" : " lines]");
      const auto pos = text.find(placeholder);
      if (pos != std::string::npos) {
        text.replace(pos, placeholder.size(), blk.content);
      }
    } else {
      // Image: strip the placeholder (or replace with a soft mention)
      // and stash the data URI in `images`.
      const std::string placeholder =
          "[Pasted #" + std::to_string(blk.id) + ": image]";
      const auto pos = text.find(placeholder);
      if (pos != std::string::npos) {
        // Keep a tiny inline tag so the message reads sensibly:
        // "look at this <image> and tell me ...". The image itself
        // arrives as a separate ImageContent.
        text.replace(pos, placeholder.size(), "<image>");
      }
      firmius::shared::ImageContent img;
      img.url = "data:" + blk.mediaType + ";base64," + blk.content;
      img.mediaType = blk.mediaType;
      images.push_back(std::move(img));
    }
  }

  if (text.empty() && images.empty()) {
    return;
  }

  state_.clearInput();
  dismissAutocomplete();

  if (text.size() > 0 && text[0] == '/' && commands_.execute(text)) {
    return;
  }

  dispatcher_.sendMessage(text, std::move(images));
}

void App::applyModelSelection(const std::string& providerId,
                              const std::string& modelId,
                              uint32_t contextWindowTokens) {
  const std::string targetAgentId =
      state_.threadId().empty() ? std::string{} : state_.agentId();
  auto agent = session_.switchModel(targetAgentId, providerId, modelId);
  if (!agent) {
    notifications_.error("Failed to switch model");
    return;
  }

  const uint32_t effectiveWindow =
      agent->contextWindowTokens > 0 ? agent->contextWindowTokens
                                     : contextWindowTokens;
  state_.updateAgentModel(state_.agentId(), providerId, modelId, effectiveWindow);
}

// ── Autocomplete ──────────────────────────────────────────────────────────
//
// Two-mode autocomplete: while the user is typing the command name, we match
// against registered command names; once the user types a space we switch to
// arg-suggestion mode and pull a list from the daemon based on the arg's
// declared ArgType.
//
// The dropdown shows up to kAutocompleteVisibleRows entries at a time. When
// `matches` is longer than that, navigation past the visible window scrolls
// — it does NOT wrap. This matches the user's expectation: pressing down at
// the bottom of a 50-item provider list should reveal item 6, not jump to
// item 1.

namespace {

bool icontains(const std::string& haystack, const std::string& needle) {
  if (needle.empty()) return true;
  auto it = std::search(
      haystack.begin(), haystack.end(),
      needle.begin(), needle.end(),
      [](char a, char b) {
        return std::tolower(static_cast<unsigned char>(a)) ==
               std::tolower(static_cast<unsigned char>(b));
      });
  return it != haystack.end();
}

bool ihasPrefix(const std::string& s, const std::string& prefix) {
  if (prefix.size() > s.size()) return false;
  for (size_t i = 0; i < prefix.size(); ++i) {
    if (std::tolower(static_cast<unsigned char>(s[i])) !=
        std::tolower(static_cast<unsigned char>(prefix[i]))) {
      return false;
    }
  }
  return true;
}

void rankAndCap(std::vector<AutocompleteMatch>& matches,
                const std::string& filter,
                size_t cap) {
  // Stable rank: prefix matches first, then substring matches. Within each
  // bucket alphabetical. Then cap to a generous limit so the list stays
  // navigable but the user has scrollback room. We keep more than the
  // visible window so scrolling does something.
  std::stable_sort(matches.begin(), matches.end(),
                   [&](const AutocompleteMatch& a, const AutocompleteMatch& b) {
                     bool ap = ihasPrefix(a.name, filter);
                     bool bp = ihasPrefix(b.name, filter);
                     if (ap != bp) return ap;
                     return a.name < b.name;
                   });
  constexpr size_t kMaxMatches = 100;
  if (matches.size() > kMaxMatches) matches.resize(kMaxMatches);
  (void)cap;
}

}  // namespace

std::vector<AutocompleteMatch>
App::fetchArgSuggestions(ArgType type, const std::string& filter) {
  std::vector<AutocompleteMatch> out;
  // The daemon may not be ready yet (eg the user opened the wizard before
  // connect succeeded). Bail silently — we'll just show no suggestions.
  if (!session_.connected()) return out;

  try {
    switch (type) {
    case ArgType::ProviderId: {
      auto catalog = session_.client().listProviders();
      for (const auto& p : catalog.providers) {
        if (!filter.empty() && !icontains(p.id, filter)) continue;
        AutocompleteMatch m;
        m.name = p.id;
        // Mark configured-ness inline so the user can tell what's already
        // set up vs. fresh.
        m.description = p.kind;
        if (p.configured) m.description += " · configured";
        out.push_back(std::move(m));
      }
      break;
    }
    case ArgType::ThreadId: {
      auto threads = session_.listThreads();
      for (const auto& t : threads) {
        if (!filter.empty() && !icontains(t.threadId, filter) &&
            !icontains(t.title, filter)) {
          continue;
        }
        AutocompleteMatch m;
        m.name = t.threadId;
        m.description = t.title;
        out.push_back(std::move(m));
      }
      break;
    }
    case ArgType::AgentId: {
      // Only meaningful when there's a focused thread. Pull the agent tree
      // for it; otherwise no suggestions.
      const std::string threadId = state_.threadId();
      if (threadId.empty()) break;
      auto tree = session_.listAgents(threadId);
      for (const auto& a : tree.agents) {
        if (!filter.empty() && !icontains(a.agentId, filter) &&
            !icontains(a.friendlyName, filter)) {
          continue;
        }
        AutocompleteMatch m;
        m.name = a.agentId;
        m.description = a.friendlyName.empty() ? a.persona : a.friendlyName;
        out.push_back(std::move(m));
      }
      break;
    }
    case ArgType::Mode: {
      auto modes = session_.client().listModes();
      for (const auto& m : modes.modes) {
        if (!filter.empty() && !icontains(m.modeId, filter) &&
            !icontains(m.name, filter)) {
          continue;
        }
        AutocompleteMatch entry;
        entry.name = m.modeId;
        entry.description = m.name;
        out.push_back(std::move(entry));
      }
      break;
    }
    case ArgType::Filepath: {
      // Best-effort: list files in the cwd (or filter's parent dir if it
      // contains a slash). Keep the listing shallow — fancy file
      // completion belongs in a dedicated tool, not here.
      std::filesystem::path base = std::filesystem::current_path();
      std::string namePart = filter;
      auto slash = filter.find_last_of("/\\");
      if (slash != std::string::npos) {
        base = std::filesystem::path(filter.substr(0, slash));
        if (!base.is_absolute()) {
          base = std::filesystem::current_path() / base;
        }
        namePart = filter.substr(slash + 1);
      }
      std::error_code ec;
      if (std::filesystem::is_directory(base, ec)) {
        for (auto it = std::filesystem::directory_iterator(base, ec);
             !ec && it != std::filesystem::directory_iterator(); ++it) {
          const auto& name = it->path().filename().string();
          if (name.empty() || name[0] == '.') continue;
          if (!namePart.empty() && !ihasPrefix(name, namePart)) continue;
          AutocompleteMatch m;
          m.name = name;
          m.description = it->is_directory(ec) ? "dir" : "file";
          out.push_back(std::move(m));
        }
      }
      break;
    }
    case ArgType::String:
    case ArgType::Number:
      // No static suggestion source.
      break;
    }
  } catch (const std::exception&) {
    // RPC failure — surface no suggestions rather than crashing the UI.
    out.clear();
  }

  rankAndCap(out, filter, kAutocompleteVisibleRows);
  return out;
}

void App::refreshAutocompleteMatches() {
  const auto input = state_.inputBuffer();
  autocomplete_.inputSnapshot = input;

  auto pos = commands_.parsePosition(input);

  if (!pos.isSlashInput) {
    autocomplete_.matches.clear();
    return;
  }

  if (pos.commandName.empty()) {
    // Still typing the command name.
    autocomplete_.mode = AutocompleteState::Mode::CommandName;
    autocomplete_.activeCommandName.clear();
    autocomplete_.currentArgIndex = -1;
    autocomplete_.argFilter.clear();
    // Strip leading '/'.
    std::string partial = input.size() > 1 ? input.substr(1) : "";
    autocomplete_.matches = commands_.autocomplete(partial);
    return;
  }

  // Command name fully typed — switch to arg-suggestion mode.
  auto cmd = commands_.getCommand(pos.commandName);
  if (!cmd || pos.currentArgIndex < 0) {
    // Unknown command, or past last declared arg, or command takes no args.
    autocomplete_.matches.clear();
    return;
  }
  const auto& argDefs = cmd->args();
  if (pos.currentArgIndex >= static_cast<int>(argDefs.size())) {
    autocomplete_.matches.clear();
    return;
  }

  autocomplete_.mode = AutocompleteState::Mode::ArgValue;
  autocomplete_.activeCommandName = pos.commandName;
  autocomplete_.currentArgIndex = pos.currentArgIndex;
  autocomplete_.argFilter = pos.currentArgFilter;
  autocomplete_.matches = fetchArgSuggestions(
      argDefs[pos.currentArgIndex].type, pos.currentArgFilter);
}

void App::updateAutocomplete() {
  refreshAutocompleteMatches();
  if (autocomplete_.matches.empty()) {
    // No matches — keep the dropdown active so the user knows the system
    // saw their input but has nothing to suggest. We just show nothing.
    autocomplete_.selectedIndex = 0;
    autocomplete_.scrollOffset = 0;
    state_.markDirtyPublic();
    return;
  }
  if (autocomplete_.selectedIndex >=
      static_cast<int>(autocomplete_.matches.size())) {
    autocomplete_.selectedIndex =
        std::max(0, static_cast<int>(autocomplete_.matches.size()) - 1);
  }
  // Re-clamp scroll so selectedIndex stays visible.
  const int total = static_cast<int>(autocomplete_.matches.size());
  if (autocomplete_.scrollOffset > total - 1) {
    autocomplete_.scrollOffset = std::max(0, total - 1);
  }
  if (autocomplete_.selectedIndex < autocomplete_.scrollOffset) {
    autocomplete_.scrollOffset = autocomplete_.selectedIndex;
  }
  if (autocomplete_.selectedIndex >=
      autocomplete_.scrollOffset + kAutocompleteVisibleRows) {
    autocomplete_.scrollOffset =
        autocomplete_.selectedIndex - kAutocompleteVisibleRows + 1;
  }
  state_.markDirtyPublic();
}

void App::dismissAutocomplete() {
  if (autocomplete_.active) {
    autocomplete_.active = false;
    autocomplete_.matches.clear();
    autocomplete_.selectedIndex = 0;
    autocomplete_.scrollOffset = 0;
    autocomplete_.inputSnapshot.clear();
    autocomplete_.activeCommandName.clear();
    autocomplete_.currentArgIndex = -1;
    autocomplete_.argFilter.clear();
    autocomplete_.mode = AutocompleteState::Mode::CommandName;
    state_.markDirtyPublic();
  }
}

void App::autocompleteMoveUp() {
  if (autocomplete_.matches.empty()) return;
  if (autocomplete_.selectedIndex > 0) {
    --autocomplete_.selectedIndex;
    if (autocomplete_.selectedIndex < autocomplete_.scrollOffset) {
      autocomplete_.scrollOffset = autocomplete_.selectedIndex;
    }
    state_.markDirtyPublic();
  }
}

void App::autocompleteMoveDown() {
  if (autocomplete_.matches.empty()) return;
  const int total = static_cast<int>(autocomplete_.matches.size());
  if (autocomplete_.selectedIndex < total - 1) {
    ++autocomplete_.selectedIndex;
    // Scroll down if cursor moved past the visible window's last row.
    if (autocomplete_.selectedIndex >=
        autocomplete_.scrollOffset + kAutocompleteVisibleRows) {
      autocomplete_.scrollOffset =
          autocomplete_.selectedIndex - kAutocompleteVisibleRows + 1;
    }
    state_.markDirtyPublic();
  }
  // At the bottom: do nothing. Don't wrap.
}

void App::autocompleteAccept() {
  if (autocomplete_.matches.empty()) return;
  const auto& match =
      autocomplete_.matches[autocomplete_.selectedIndex];

  if (autocomplete_.mode == AutocompleteState::Mode::CommandName) {
    // Replace the whole input with `/<name> ` so the user can immediately
    // start typing the first arg (and we can show arg suggestions).
    state_.clearInput();
    std::string replacement = "/" + match.name;
    auto cmd = commands_.getCommand(match.name);
    if (cmd && !cmd->args().empty()) {
      replacement += " ";
    }
    for (char ch : replacement) state_.appendToInput(ch);
    // If the command has args, keep the dropdown alive so the user sees
    // arg suggestions immediately.
    if (cmd && !cmd->args().empty()) {
      autocomplete_.selectedIndex = 0;
      autocomplete_.scrollOffset = 0;
      updateAutocomplete();
      return;
    }
    dismissAutocomplete();
    return;
  }

  // ArgValue mode: replace ONLY the current arg's partial text with the
  // chosen value. The rest of the input (command name, prior args) stays
  // intact. We rebuild the input by chopping `argFilter.size()` chars off
  // the end and appending `match.name + " "`.
  std::string current = state_.inputBuffer();
  if (autocomplete_.argFilter.size() <= current.size()) {
    current.resize(current.size() - autocomplete_.argFilter.size());
  }
  current += match.name;
  // Add a trailing space to invite the next arg (if any).
  current += " ";

  state_.clearInput();
  for (char ch : current) state_.appendToInput(ch);

  // Move on to the next arg's suggestions.
  autocomplete_.selectedIndex = 0;
  autocomplete_.scrollOffset = 0;
  updateAutocomplete();
}

} // namespace firmius::tui

#include "App.hpp"
#include "AnsiParser.hpp"
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

namespace firmius::tui2 {

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
      bottomBar_(state_) {}

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
    const bool highRefreshInput =
        state_.hasLiveItems() || !state_.liveMessage().empty() ||
        state_.activityContext() == ActivityContext::Active;
    const int inputTimeoutMs = highRefreshInput ? 80 : 120;
    auto [key, mouseEvent] = terminal_.readInput(inputTimeoutMs);
    if (mouseEvent.has_value()) {
      handleMouse(mouseEvent.value());
    } else if (!key.empty()) {
      handleInput(key);
    }

    // Reconcile runtime state even if the final daemon event was missed.
    reconcileRuntimeState();

    // Live tick — keep tool presenters and status animations responsive.
    const bool animatePinnedUi =
        state_.hasLiveItems() || !state_.liveMessage().empty() ||
        state_.activityContext() == ActivityContext::Active;
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
                                 autocomplete_.prefix = state_.inputBuffer();
                                 if (autocomplete_.prefix.empty() ||
                                     autocomplete_.prefix[0] != '/') {
                                   dismissAutocomplete();
                                 } else {
                                   autocomplete_.selectedIndex = 0;
                                   updateAutocomplete();
                                 }
                               }
                             }});
  keybinds_.registerKeybind({keys::kBackspaceDel, "Delete", ActivityContext::Idle,
                             true, [this]() {
                               state_.backspaceInput();
                               if (autocomplete_.active) {
                                 autocomplete_.prefix = state_.inputBuffer();
                                 if (autocomplete_.prefix.empty() ||
                                     autocomplete_.prefix[0] != '/') {
                                   dismissAutocomplete();
                                 } else {
                                   autocomplete_.selectedIndex = 0;
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
}

void App::handleInput(const std::string& key) {
  auto context = state_.activityContext();

  if (context == ActivityContext::Active && !key.empty() &&
      static_cast<unsigned char>(key[0]) == 0x1b) {
    state_.addItem(std::make_unique<SystemNoticeItem>("debug esc seen"));
    dispatcher_.interruptAgent(true);
    return;
  }

  if (context == ActivityContext::PermissionPending) {
    if (key == "y" || key == "Y") {
      auto perm = state_.pendingPermission();
      if (perm) dispatcher_.resolvePermission(perm->requestId,
                                               firmius::shared::PermissionResponse::AllowOnce);
      return;
    }
    if (key == "n" || key == "N") {
      auto perm = state_.pendingPermission();
      if (perm) dispatcher_.resolvePermission(perm->requestId,
                                               firmius::shared::PermissionResponse::Deny);
      return;
    }
    if (key == "a" || key == "A") {
      auto perm = state_.pendingPermission();
      if (perm) dispatcher_.resolvePermission(perm->requestId,
                                               firmius::shared::PermissionResponse::AllowAlways);
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
    if (key == "\t" || key == "\r" || key == "\n") {
      autocompleteAccept();
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

  if (keybinds_.handleKey(key, context)) {
    return;
  }

  for (unsigned char ch : key) {
    if (ch == '\r' || ch == '\n' || ch == 0x7f || ch == '\b') {
      keybinds_.handleKey(std::string(1, static_cast<char>(ch)), context);
    } else if (ch >= 32 && ch < 127) {
      state_.appendToInput(static_cast<char>(ch));
      // Trigger autocomplete if input starts with '/'.
      if (state_.inputBuffer().size() == 1 && ch == '/') {
        autocomplete_.active = true;
        autocomplete_.prefix = "/";
        autocomplete_.selectedIndex = 0;
        updateAutocomplete();
      } else if (autocomplete_.active) {
        // Update autocomplete as user types.
        autocomplete_.prefix = state_.inputBuffer();
        autocomplete_.selectedIndex = 0;
        updateAutocomplete();
        if (autocomplete_.prefix.find(' ') != std::string::npos ||
            autocomplete_.matches.empty()) {
          dismissAutocomplete();
        }
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

  // No overlay active — scroll transcript.
  if (event.type == MouseEvent::Type::Scroll) {
    if (event.button == MouseEvent::Button::ScrollUp) {
      state_.scrollUp(3);
    } else if (event.button == MouseEvent::Button::ScrollDown) {
      state_.scrollDown(3);
    }
  }
}

void App::reconcileRuntimeState() {
  if (state_.agentStatus() != firmius::shared::AgentStatus::Streaming) return;

  const auto now = std::chrono::steady_clock::now();
  if (lastRuntimeReconcile_ != std::chrono::steady_clock::time_point{} &&
      now - lastRuntimeReconcile_ < std::chrono::milliseconds(500)) {
    return;
  }
  lastRuntimeReconcile_ = now;

  const auto threadId = state_.threadId();
  const auto agentId = state_.agentId();
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

  // Autocomplete dropdown height (0 when inactive).
  int autocompleteH = autocomplete_.active && !autocomplete_.matches.empty()
                          ? std::min(5, static_cast<int>(autocomplete_.matches.size()))
                          : 0;

  const auto liveLines = statusBar_.renderLiveSection(w);
  const auto overlayLines =
      activeOverlay_ ? activeOverlay_->render(w) : std::vector<std::string>{};
  const auto inputLines = inputBar_.render(w);
  const auto agentLines = state_.hasMultipleAgents() ? agentTabBar_.render(w)
                                                     : std::vector<std::string>{};
  const auto hudLines = statusBar_.renderHudSection(w);
  const auto bottomLines = bottomBar_.render(w);

  std::vector<std::string> pinnedLines;
  pinnedLines.reserve(liveLines.size() + overlayLines.size() + inputLines.size() +
                      agentLines.size() + hudLines.size() + bottomLines.size());
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

  // Draw autocomplete rows directly under the transcript.
  if (autocompleteH > 0) {
    int acStart = transcriptH;
    for (int i = 0; i < autocompleteH; ++i) {
      int row = acStart + i;
      if (row < 0 || row >= h) continue;
      const auto& match = autocomplete_.matches[i];
      bool selected = (i == autocomplete_.selectedIndex);
      std::string line = "  " + match.name;
      if (!match.description.empty()) {
        line += "  " + ansi::dim(match.description);
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
  {
    int cursorRow = pinnedStart + static_cast<int>(liveLines.size()) +
                    static_cast<int>(overlayLines.size()) +
                    inputBar_.cursorRowOffset();
    auto input = state_.inputBuffer();
    terminal_.moveCursor(cursorRow, 5 + static_cast<int>(input.size()));
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

  // Fill target grid with parsed ANSI cells.
  for (int i = startLine; i < endLine; ++i) {
    int row = i - startLine; // 0-indexed row in target
    if (row >= transcriptH) break;
    auto cells = AnsiParser::parse(scrollback[i], w);
    for (int c = 0; c < std::min(static_cast<int>(cells.size()), w); ++c) {
      applyFallbackBackground(cells[c], theme);
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

    // Subscribe to events — init progress messages will arrive as SystemNoticeItems.
    session_.subscribe([this](const firmius::daemon::DaemonEventEnvelope& envelope) {
      eventRouter_.route(envelope);
    });

    if (!options_.threadId.empty()) {
      dispatcher_.openThread(options_.threadId);
    } else if (options_.continueSession) {
      // TODO: resume last session.
    } else {
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
    state_.addItem(std::make_unique<SystemNoticeItem>(
        std::string("Failed to list models: ") + e.what()));
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
    state_.addItem(std::make_unique<SystemNoticeItem>(
        std::string("Failed to list threads: ") + e.what()));
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
    state_.addItem(std::make_unique<SystemNoticeItem>(
        std::string("Failed to fetch accounts: ") + e.what()));
  }
}

void App::applyTheme(const std::string& name) {
  if (!ThemeManager::instance().setTheme(name)) {
    state_.addItem(std::make_unique<SystemNoticeItem>(
        "Unknown theme: " + name));
    return;
  }
  prevFrame_.clear();
  prevW_ = 0;
  prevH_ = 0;
  state_.clearScrollback();
  lastSyncedItemCount_ = 0;
  lastSyncedRowCounts_.clear();
  state_.markDirtyPublic();
  state_.addItem(std::make_unique<SystemNoticeItem>("Theme changed to " + name));
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

void App::submitInputBuffer() {
  if (!state_.daemonReady()) {
    return;
  }
  auto text = state_.inputBuffer();
  if (text.empty()) {
    return;
  }

  state_.clearInput();
  dismissAutocomplete();

  if (text[0] == '/' && commands_.execute(text)) {
    return;
  }

  dispatcher_.sendMessage(text);
}

void App::applyModelSelection(const std::string& providerId,
                              const std::string& modelId,
                              uint32_t contextWindowTokens) {
  const std::string targetAgentId =
      state_.threadId().empty() ? std::string{} : state_.agentId();
  auto agent = session_.switchModel(targetAgentId, providerId, modelId);
  if (!agent) {
    state_.addItem(
        std::make_unique<SystemNoticeItem>("Failed to switch model"));
    return;
  }

  const uint32_t effectiveWindow =
      agent->contextWindowTokens > 0 ? agent->contextWindowTokens
                                     : contextWindowTokens;
  state_.updateAgentModel(state_.agentId(), providerId, modelId, effectiveWindow);
}

// ── Autocomplete ──

void App::updateAutocomplete() {
  autocomplete_.matches = commands_.autocomplete(
      autocomplete_.prefix.size() > 1 ? autocomplete_.prefix.substr(1) : "");
  // Limit to 5 visible.
  if (autocomplete_.matches.size() > 5) {
    autocomplete_.matches.resize(5);
  }
  if (autocomplete_.selectedIndex >= static_cast<int>(autocomplete_.matches.size())) {
    autocomplete_.selectedIndex = std::max(0, static_cast<int>(autocomplete_.matches.size()) - 1);
  }
  state_.markDirtyPublic();
}

void App::dismissAutocomplete() {
  if (autocomplete_.active) {
    autocomplete_.active = false;
    autocomplete_.matches.clear();
    autocomplete_.selectedIndex = 0;
    autocomplete_.prefix.clear();
    state_.markDirtyPublic();
  }
}

void App::autocompleteMoveUp() {
  if (autocomplete_.selectedIndex > 0) {
    --autocomplete_.selectedIndex;
    state_.markDirtyPublic();
  }
}

void App::autocompleteMoveDown() {
  if (autocomplete_.selectedIndex < static_cast<int>(autocomplete_.matches.size()) - 1) {
    ++autocomplete_.selectedIndex;
    state_.markDirtyPublic();
  }
}

void App::autocompleteAccept() {
  if (autocomplete_.matches.empty()) return;
  const auto& match = autocomplete_.matches[autocomplete_.selectedIndex];
  // Replace input with the selected command.
  state_.clearInput();
  for (char ch : "/" + match.name) {
    state_.appendToInput(ch);
  }
  dismissAutocomplete();
}

} // namespace firmius::tui2

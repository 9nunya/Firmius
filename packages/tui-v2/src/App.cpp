#include "App.hpp"
#include "AnsiParser.hpp"
#include "Terminal.hpp"
#include "WorkflowCommand.hpp"
#include "items/SimpleItems.hpp"
#include "items/StreamingItems.hpp"
#include "items/ToolCallItem.hpp"
#include "tools/PresenterInit.hpp"
#include "workflow/WorkflowLoader.hpp"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <thread>

namespace firmius::tui2 {

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

  // Set up pinned zone: statusBar (2) + inputBar (1) = 3 lines.
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
    auto [key, mouseEvent] = terminal_.readInput(30);
    if (mouseEvent.has_value()) {
      handleMouse(mouseEvent.value());
    } else if (!key.empty()) {
      handleInput(key);
    }

    // Reconcile runtime state even if the final daemon event was missed.
    reconcileRuntimeState();

    // Live tick — mark live items dirty every 500ms.
    if (state_.hasLiveItems()) {
      auto now = std::chrono::steady_clock::now();
      if (now - lastLiveTick > std::chrono::milliseconds(500)) {
        lastLiveTick = now;
        state_.markLiveItemsDirty();
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

  // Load workflow definitions from disk and register as slash commands.
  firmius::core::WorkflowLoader::instance().init();
  registerWorkflowCommands(commands_, dispatcher_);
}

void App::setupKeybinds() {
  keybinds_.registerKeybind({keys::kCtrlQ, "Quit", ActivityContext::Idle, true,
                             [this]() { running_ = false; }});

  keybinds_.registerKeybind({keys::kCtrlC, "Quit", ActivityContext::Idle, true,
                             [this]() { running_ = false; }});
  keybinds_.registerKeybind({keys::kCtrlC, "Interrupt", ActivityContext::Active,
                             false, [this]() { dispatcher_.interruptAgent(); }});

  keybinds_.registerKeybind({keys::kEscape, "Interrupt", ActivityContext::Active,
                             false, [this]() { dispatcher_.interruptAgent(); }});
  keybinds_.registerKeybind({keys::kEscape, "Dismiss", ActivityContext::Idle,
                             true, [this]() {
                               if (menu_.isActive()) { dismissMenu(); return; }
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
                               if (menu_.isActive()) {
                                 menu_.selectCurrent();
                                 return;
                               }
                               if (autocomplete_.active) {
                                 autocompleteAccept();
                                 return;
                               }
                               auto text = state_.inputBuffer();
                               if (text.empty()) return;
                               state_.clearInput();
                               dismissAutocomplete();
                               if (text[0] == '/' && commands_.execute(text)) {
                                 return;
                               }
                               state_.addItem(std::make_unique<UserMessageItem>(text));
                               dispatcher_.sendMessage(text);
                             }});

  keybinds_.registerKeybind({keys::kUp, "Up", ActivityContext::Idle, true,
                             [this]() {
                               if (autocomplete_.active) {
                                 autocompleteMoveUp();
                                 return;
                               }
                               if (menu_.isActive()) {
                                 menu_.moveUp();
                                 state_.markDirtyPublic();
                               }
                             }});
  keybinds_.registerKeybind({keys::kDown, "Down", ActivityContext::Idle, true,
                             [this]() {
                               if (autocomplete_.active) {
                                 autocompleteMoveDown();
                                 return;
                               }
                               if (menu_.isActive()) {
                                 menu_.moveDown();
                                 state_.markDirtyPublic();
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
}

void App::handleInput(const std::string& key) {
  auto context = state_.activityContext();

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

  // When menu is active, route input to menu.
  if (menu_.isActive()) {
    // Bare Escape (not an arrow key sequence) — dismiss menu.
    if (key == "\x1b") {
      dismissMenu();
      return;
    }
    if (key == "\r" || key == "\n") {
      menu_.selectCurrent();
      dismissMenu();
      return;
    }
    if (key == "\x7f" || key == "\b") {
      menu_.backspaceSearch();
      state_.markDirtyPublic();
      return;
    }
    // Arrow keys — let keybinds handle menu navigation.
    if (keybinds_.handleKey(key, context)) {
      return;
    }
    // Printable characters — append to search.
    for (unsigned char ch : key) {
      if (ch >= 32 && ch < 127) {
        menu_.appendToSearch(static_cast<char>(ch));
        state_.markDirtyPublic();
      }
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
  int statusBarH = 2;
  int pinnedH = layout_.pinnedHeight(w);
  // MouseEvent::row is 1-indexed. menuStartRow in 1-indexed coordinates.
  int menuStartRow1 = h - pinnedH + statusBarH + 1;
  int menuH = menu_.isActive() ? menu_.height(w) : 0;

  // Check if mouse is inside the menu list (1-indexed coords).
  bool insideMenu = menu_.isActive() &&
                    event.row >= menuStartRow1 &&
                    event.row < menuStartRow1 + menuH;

  // Mouse move — update hover state.
  if (event.type == MouseEvent::Type::Move) {
    if (insideMenu) {
      int itemRow = (event.row - menuStartRow1) - 3;
      if (itemRow >= 0 && itemRow < menu_.itemsAreaHeight()) {
        menu_.setHoveredIndex(menu_.firstVisibleIndex() + itemRow);
      } else {
        menu_.setHoveredIndex(-1);
      }
      state_.markDirtyPublic();
    } else {
      if (menu_.hoveredIndex() != -1) {
        menu_.setHoveredIndex(-1);
        state_.markDirtyPublic();
      }
    }
    return;
  }

  if (event.type == MouseEvent::Type::Scroll) {
    if (insideMenu) {
      // Scroll inside menu — scroll up moves cursor down, scroll down moves up
      // (natural scroll: scrolling up reveals items below).
      if (event.button == MouseEvent::Button::ScrollUp) {
        menu_.moveDown();
      } else if (event.button == MouseEvent::Button::ScrollDown) {
        menu_.moveUp();
      }
      state_.markDirtyPublic();
    } else {
      // Scroll outside menu — scroll transcript.
      if (event.button == MouseEvent::Button::ScrollUp) {
        state_.scrollUp(3);
      } else if (event.button == MouseEvent::Button::ScrollDown) {
        state_.scrollDown(3);
      }
    }
    return;
  }

  if (event.type == MouseEvent::Type::Press &&
      event.button == MouseEvent::Button::Left) {
    if (insideMenu) {
      // Click inside menu — first click highlights, second click on same item enters.
      // Menu layout: title(1) + separator(1) + search(1) + items...
      int itemRow = (event.row - menuStartRow1) - 3;
      if (itemRow >= 0 && itemRow < menu_.itemsAreaHeight()) {
        int targetIndex = menu_.firstVisibleIndex() + itemRow;
        int prevIndex = menu_.selectedIndex();
        // Navigate to the target index.
        while (menu_.selectedIndex() < targetIndex) menu_.moveDown();
        while (menu_.selectedIndex() > targetIndex) menu_.moveUp();
        // If clicking on the already-focused item, enter it.
        if (prevIndex == targetIndex) {
          menu_.selectCurrent();
          dismissMenu();
        }
        state_.markDirtyPublic();
      }
      return;
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
  // Global items (no agentId) are always shown
  auto type = item.type();
  if (type == "UserMessage" || type == "SystemNotice" || type == "ErrorMessage") {
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
  // Force scrollback rebuild
  lastFocusedAgentId_ = agentId;
  state_.clearScrollback();
  lastSyncedItemCount_ = 0;
  lastSyncedRowCounts_.clear();
  state_.scrollToBottom();

  // Re-point active streaming items to the new agent's last unfinalized items
  state_.setActiveTextItem(nullptr);
  state_.setActiveThinkingItem(nullptr);
  const auto& items = state_.items();
  for (auto it = items.rbegin(); it != items.rend(); ++it) {
    if ((*it)->type() == "AgentText" && !(*it)->isFinalized()) {
      auto* text = static_cast<AgentTextItem*>(it->get());
      if (text->agentId() == agentId) {
        state_.setActiveTextItem(text);
        break;
      }
    }
  }
  for (auto it = items.rbegin(); it != items.rend(); ++it) {
    if ((*it)->type() == "AgentThinking" && !(*it)->isFinalized()) {
      auto* think = static_cast<AgentThinkingItem*>(it->get());
      if (think->agentId() == agentId) {
        state_.setActiveThinkingItem(think);
        break;
      }
    }
  }
}

// ── Full-Screen Rendering Pipeline ──

void App::renderFrame() {
  auto [w, h] = terminal_.size();

  // Autocomplete dropdown height (0 when inactive).
  int autocompleteH = autocomplete_.active && !autocomplete_.matches.empty()
                          ? std::min(5, static_cast<int>(autocomplete_.matches.size()))
                          : 0;

  // Update pinned component composition.
  // bottomBar is always rendered manually (after autocomplete rows when active).
  if (menu_.isActive()) {
    layout_.setPinnedComponents({&statusBar_, &agentTabBar_, &menu_, &inputBar_});
  } else if (state_.hasMultipleAgents()) {
    layout_.setPinnedComponents({&statusBar_, &agentTabBar_, &inputBar_});
  } else {
    layout_.setPinnedComponents({&statusBar_, &inputBar_});
  }

  int basePinnedH = layout_.pinnedHeight(w);
  // Total pinned height = statusBar + menu + inputBar + autocomplete + bottomBar.
  int pinnedH = basePinnedH + autocompleteH + 1; // +1 for bottomBar

  // Render transcript shorter to leave room for autocomplete + pinned zone.
  int transcriptH = h - pinnedH;
  if (transcriptH < 1) transcriptH = 1;

  CellGrid target(h, std::vector<Cell>(w));
  renderTranscriptZone(target, w, transcriptH);

  // Layout (bottom-up):
  //   statusBar(2) + menu(mH) + inputBar(1)  ← pinned zone, at bottom
  //   autocomplete rows (0 or more)           ← between transcript and pinned
  //   bottomBar(1)                            ← between transcript and autocomplete
  //   transcript                              ← fills remaining space

  // Render pinned zone (statusBar + menu + inputBar) at the very bottom.
  int sbH = 2;
  int mH = menu_.isActive() ? menu_.height(w) : 0;
  renderPinnedZone(target, w, h, basePinnedH);

  // bottomBar and autocomplete render between transcript and pinned zone.
  // They start right after the transcript ends (row transcriptH).
  int gapStart = transcriptH; // first row after transcript

  // Draw bottomBar at gapStart.
  {
    auto bbLines = bottomBar_.render(w);
    if (!bbLines.empty() && gapStart >= 0 && gapStart < h) {
      auto cells = AnsiParser::parse(bbLines[0], w);
      for (int c = 0; c < std::min(static_cast<int>(cells.size()), w); ++c) {
        target[gapStart][c] = cells[c];
      }
    }
  }

  // Draw autocomplete rows below bottomBar.
  if (autocompleteH > 0) {
    int acStart = gapStart + 1; // autocomplete starts after bottomBar
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
        line = ansi::bgRgb(40, 40, 55, ansi::fitToWidth(line, w));
      } else {
        line = ansi::fitToWidth(line, w);
      }
      auto cells = AnsiParser::parse(line, w);
      for (int c = 0; c < std::min(static_cast<int>(cells.size()), w); ++c) {
        target[row][c] = cells[c];
      }
    }
  }

  // Set menu screen row for mouse hit testing.
  {
    int menuStartRow = h - basePinnedH + sbH;
    menu_.setScreenRow(menuStartRow);
  }

  // Diff against previous frame and emit only changed cells.
  terminal_.beginBatch();
  terminal_.hideCursor();
  diffAndEmit(target, w, h);

  // Position cursor at the end of the input bar for typing.
  // basePinnedH = statusBar(2) + menu(mH) + inputBar(1).
  // inputBar row (1-indexed) = h - basePinnedH + sbH + mH + 1.
  {
    int cursorRow = h - basePinnedH + sbH + mH + 1;
    auto input = state_.inputBuffer();
    terminal_.moveCursor(cursorRow, 4 + static_cast<int>(input.size()));
  }
  terminal_.showCursor();
  terminal_.flushBatch();

  // Save frame for next diff.
  prevFrame_ = std::move(target);
  prevW_ = w;
  prevH_ = h;
}

void App::renderTranscriptZone(CellGrid& target, int w, int transcriptH) {
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
      target[row][c] = cells[c];
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
      target[0][startCol + c] = indCells[c];
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
    // Show immediate feedback — the user sees this on the first frame.
    state_.addItem(std::make_unique<SystemNoticeItem>("Connecting to daemon..."));

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
      state_.addItem(std::make_unique<SystemNoticeItem>(
          "Daemon not ready, retrying in " + std::to_string(delayMs / 1000.0) + "s... (attempt " +
          std::to_string(attempt + 1) + "/" + std::to_string(maxTotalAttempts) + ")"));
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

    state_.addItem(std::make_unique<SystemNoticeItem>("Connected. Subscribing to events..."));
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
      state_.addItem(std::make_unique<SystemNoticeItem>("Loading UI snapshot..."));
      firmius::daemon::UiSnapshotRequest request;
      // uiSnapshot now blocks until daemon init is complete (up to 30s).
      firmius::daemon::UiSnapshot uiSnap = session_.client().uiSnapshot(request);
      std::string label = uiSnap.config.config.defaultProviderId + "/" +
                          uiSnap.config.config.defaultModelId;
      state_.setModelLabel(label);
      state_.setAgentPurpose(uiSnap.config.config.defaultLeadPersona);
      if (uiSnap.config.config.defaultMaxTokens.has_value()) {
        state_.setAgentContextWindow(
            std::to_string(uiSnap.config.config.defaultMaxTokens.value() / 1000) + "k ctx");
      }
      state_.addItem(std::make_unique<SystemNoticeItem>("UI snapshot loaded."));
    }

    // Welcome message.
    auto threadId = state_.threadId();
    std::string welcomeText = threadId.empty()
        ? "Ready. Type a message or use /new to start."
        : "Ready. Thread: " + threadId;
    state_.addItem(std::make_unique<SystemNoticeItem>(welcomeText));

  } catch (const std::exception& e) {
    state_.setConnectionStatus(ConnectionStatus::Disconnected);
    state_.addItem(std::make_unique<ErrorMessageItem>(e.what()));
  }
}

void App::openModelsMenu() {
  try {
    auto models = session_.client().listModels(true);
    std::vector<MenuList::Item> items;
    for (const auto& m : models.models) {
      MenuList::Item item;
      item.label = m.id;
      item.detail = m.provider;
      item.id = m.provider + ":" + m.id;
      items.push_back(std::move(item));
    }
    menu_.open();
    menu_.setTitle("Select Model");
    menu_.setItems(std::move(items));
    menu_.setOnSelect([this](const MenuList::Item& item) {
      auto sep = item.id.find(':');
      if (sep != std::string::npos) {
        auto providerId = item.id.substr(0, sep);
        auto modelId = item.id.substr(sep + 1);
        auto agent = session_.switchModel(state_.agentId(), providerId, modelId);
        if (agent) {
          state_.setModelLabel(providerId + "/" + modelId);
        } else {
          state_.addItem(std::make_unique<SystemNoticeItem>("Failed to switch model"));
        }
      }
      dismissMenu();
    });
    menu_.setOnDismiss([this]() { dismissMenu(); });
    state_.markDirtyPublic();
  } catch (const std::exception& e) {
    state_.addItem(std::make_unique<SystemNoticeItem>(
        std::string("Failed to list models: ") + e.what()));
  }
}

void App::openResumeMenu() {
  try {
    auto threads = session_.listThreads();
    std::vector<MenuList::Item> items;
    for (const auto& t : threads) {
      MenuList::Item item;
      item.label = t.title.empty() ? t.threadId : t.title;
      item.detail = t.threadId.substr(0, 8) + "...";
      item.id = t.threadId;
      items.push_back(std::move(item));
    }
    menu_.open();
    menu_.setTitle("Resume Session");
    menu_.setItems(std::move(items));
    menu_.setOnSelect([this](const MenuList::Item& item) {
      dispatcher_.openThread(item.id);
      dismissMenu();
    });
    menu_.setOnDismiss([this]() { dismissMenu(); });
    state_.markDirtyPublic();
  } catch (const std::exception& e) {
    state_.addItem(std::make_unique<SystemNoticeItem>(
        std::string("Failed to list threads: ") + e.what()));
  }
}

void App::dismissMenu() {
  if (menu_.isActive()) {
    menu_.setHoveredIndex(-1);
    menu_.close();
    state_.markDirtyPublic();
  }
  dismissAutocomplete();
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

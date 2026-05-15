#include "App.hpp"

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
      layout_(terminal_),
      eventRouter_(state_),
      dispatcher_(session_, state_),
      transcriptRenderer_(terminal_),
      statusBar_(state_),
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

  // Register commands and keybinds.
  registerCommands();
  setupKeybinds();

  // Set up pinned zone: statusBar (2) + inputBar (1) + bottomBar (1) = 4 lines.
  layout_.setPinnedComponents({&statusBar_, &inputBar_, &bottomBar_});
  layout_.renderPinned();

  // Connect to daemon.
  state_.setConnectionStatus(ConnectionStatus::Connecting);
  layout_.renderPinned();
  connectAndSetup();

  // Main loop.
  running_ = true;
  while (running_) {
    // Check for resize.
    if (terminal_.wasResized()) {
      onResize();
    }

    // Read input (non-blocking).
    std::string key = terminal_.readKey(30);
    if (!key.empty()) {
      handleInput(key);
    }

    // Re-render pinned zone if state changed.
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
  // /quit
  commands_.registerCommand(std::make_shared<class QuitCmd>(*this));
  // /new
  commands_.registerCommand(std::make_shared<class NewCmd>(*this));
  // /models
  commands_.registerCommand(std::make_shared<class ModelsCmd>(*this));
  // /resume
  commands_.registerCommand(std::make_shared<class ResumeCmd>(*this));

  // TODO: autoload workflow commands from WorkflowLoader.
}

void App::setupKeybinds() {
  // Ctrl+Q = Quit.
  keybinds_.registerKeybind({keys::kCtrlQ, "Quit", ActivityContext::Idle, true,
                             [this]() { running_ = false; }});

  // Ctrl+C = Quit when idle, interrupt when streaming.
  keybinds_.registerKeybind({keys::kCtrlC, "Quit", ActivityContext::Idle, true,
                             [this]() { running_ = false; }});
  keybinds_.registerKeybind({keys::kCtrlC, "Interrupt", ActivityContext::Streaming,
                             false, [this]() { dispatcher_.interruptAgent(); }});

  // Escape = interrupt when streaming, dismiss menu when idle.
  keybinds_.registerKeybind({keys::kEscape, "Interrupt", ActivityContext::Streaming,
                             false, [this]() { dispatcher_.interruptAgent(); }});
  keybinds_.registerKeybind({keys::kEscape, "Dismiss", ActivityContext::Idle,
                             true, [this]() {
                               if (menu_.isActive()) { dismissMenu(); }
                             }});

  // Ctrl+N = New thread.
  keybinds_.registerKeybind({keys::kCtrlN, "New Thread", ActivityContext::Idle,
                             false, [this]() {
                               dispatcher_.createThread(options_.persona, options_.mode);
                             }});

  // Enter = Send message (or select menu item).
  keybinds_.registerKeybind({keys::kEnter, "Send", ActivityContext::Idle, false,
                             [this]() {
                               if (menu_.isActive()) {
                                 menu_.selectCurrent();
                                 return;
                               }
                               auto text = state_.inputBuffer();
                               if (text.empty()) return;
                               state_.clearInput();
                               // Try as command first.
                               if (text[0] == '/' && commands_.execute(text)) {
                                 return;
                               }
                               // Otherwise send as message.
                               dispatcher_.sendMessage(text);
                             }});

  // Arrow keys for menu navigation.
  keybinds_.registerKeybind({keys::kUp, "Up", ActivityContext::Idle, true,
                             [this]() {
                               if (menu_.isActive()) {
                                 menu_.moveUp();
                                 state_.markDirtyPublic();
                               }
                             }});
  keybinds_.registerKeybind({keys::kDown, "Down", ActivityContext::Idle, true,
                             [this]() {
                               if (menu_.isActive()) {
                                 menu_.moveDown();
                                 state_.markDirtyPublic();
                               }
                             }});

  // Backspace.
  keybinds_.registerKeybind({keys::kBackspace, "Delete", ActivityContext::Idle,
                             true, [this]() { state_.backspaceInput(); }});
  keybinds_.registerKeybind({keys::kBackspaceDel, "Delete", ActivityContext::Idle,
                             true, [this]() { state_.backspaceInput(); }});
}

void App::handleInput(const std::string& key) {
  auto context = state_.activityContext();

  // Permission handling.
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
    return; // Swallow all other input during permission prompts.
  }

  // Try registered keybinds.
  if (keybinds_.handleKey(key, context)) {
    return;
  }

  // Printable character → append to input.
  if (key.size() == 1 && key[0] >= 32 && key[0] < 127) {
    state_.appendToInput(key[0]);
  }
}

void App::renderFrame() {
  // Push any new transcript lines into the scroll zone.
  auto lines = state_.transcriptLines();
  size_t lastRendered = state_.lastRenderedLineIndex();
  if (lastRendered < lines.size()) {
    std::vector<std::string> newLines;
    for (size_t i = lastRendered; i < lines.size(); ++i) {
      newLines.push_back(transcriptRenderer_.formatLine(lines[i], layout_.width()));
    }
    layout_.pushTranscriptLines(newLines);
    state_.setLastRenderedLineIndex(lines.size());
  }

  // Render streaming text progressively.
  auto streaming = state_.currentStreamingText();
  if (!streaming.empty() && streaming != lastStreamingText_) {
    // Find new content since last render.
    std::string newContent;
    if (streaming.size() > lastStreamingText_.size() &&
        streaming.substr(0, lastStreamingText_.size()) == lastStreamingText_) {
      newContent = streaming.substr(lastStreamingText_.size());
    } else {
      newContent = streaming;
    }

    // Split on newlines and push each complete line.
    size_t pos = 0;
    while ((pos = newContent.find('\n')) != std::string::npos) {
      std::string chunk = newContent.substr(0, pos);
      if (!chunk.empty()) {
        layout_.pushTranscriptLine(
            ansi::fgRgb(220, 220, 230, chunk));
      }
      newContent = newContent.substr(pos + 1);
    }

    // Render partial line (no newline yet) by overwriting the last
    // scroll row. For simplicity, just show the latest word.
    if (!newContent.empty()) {
      // We'll push the partial when next newline comes or on finalize.
    }

    lastStreamingText_ = streaming;
  }

  // If streaming was just finalized, clear tracking.
  if (streaming.empty() && !lastStreamingText_.empty()) {
    lastStreamingText_.clear();
  }

  // Update pinned components.
  if (menu_.isActive()) {
    layout_.setPinnedComponents({&statusBar_, &menu_, &inputBar_, &bottomBar_});
  } else {
    layout_.setPinnedComponents({&statusBar_, &inputBar_, &bottomBar_});
  }
  layout_.renderPinned();

  // Position cursor at the end of the input bar for typing.
  int inputRow = terminal_.pinnedTopRow() + 2; // StatusBar is 2 lines, input is next.
  if (menu_.isActive()) {
    inputRow = terminal_.pinnedTopRow() + 2 + menu_.height(layout_.width());
  }
  auto input = state_.inputBuffer();
  terminal_.moveCursor(inputRow, 4 + static_cast<int>(input.size()));
  terminal_.showCursor();
}

void App::onResize() {
  // Clear the entire screen to prevent ghost rows.
  terminal_.clearScreen();
  layout_.invalidate();

  // Re-render the pinned zone at the new positions.
  layout_.renderPinned();

  // Re-push recent transcript so it's visible again.
  auto lines = state_.transcriptLines();
  int w = layout_.width();
  // Show last N lines that fit in the scroll area.
  int scrollHeight = layout_.height() - terminal_.pinnedHeight();
  int start = std::max(0, static_cast<int>(lines.size()) - scrollHeight);
  for (int i = start; i < static_cast<int>(lines.size()); ++i) {
    layout_.pushTranscriptLine(transcriptRenderer_.formatLine(lines[i], w));
  }
}

void App::connectAndSetup() {
  try {
    if (!session_.connect()) {
      state_.setConnectionStatus(ConnectionStatus::Disconnected);
      TranscriptLine line;
      line.kind = TranscriptLine::Kind::Notice;
      line.text = "⚠ Failed to connect to daemon. Is firmiusd running?";
      state_.appendTranscriptLine(std::move(line));
      return;
    }

    state_.setConnectionStatus(ConnectionStatus::Connected);

    // Subscribe to events.
    session_.subscribe([this](const firmius::daemon::DaemonEventEnvelope& envelope) {
      eventRouter_.route(envelope);
    });

    // Open existing thread or create new one.
    if (!options_.threadId.empty()) {
      dispatcher_.openThread(options_.threadId);
    } else if (options_.continueSession) {
      // TODO: resume last session.
    } else {
      // Set the default model for the welcome screen
      auto uiSnap = session_.client().uiSnapshot(firmius::daemon::UiSnapshotRequest());
      std::string label = uiSnap.config.config.defaultProviderId + "/" + 
                          uiSnap.config.config.defaultModelId;
      state_.setModelLabel(label);
      state_.setAgentPurpose(uiSnap.config.config.defaultLeadPersona);
      if (uiSnap.config.config.defaultMaxTokens.has_value()) {
        state_.setAgentContextWindow(std::to_string(uiSnap.config.config.defaultMaxTokens.value() / 1000) + "k ctx");
      }
    }

    // Welcome message.
    TranscriptLine line;
    line.kind = TranscriptLine::Kind::System;
    auto threadId = state_.threadId();
    line.text = threadId.empty()
        ? "Connected to firmiusd. Type a message or use /new to start."
        : "Connected. Thread: " + threadId;
    state_.appendTranscriptLine(std::move(line));

  } catch (const std::exception& e) {
    state_.setConnectionStatus(ConnectionStatus::Disconnected);
    TranscriptLine line;
    line.kind = TranscriptLine::Kind::Notice;
    line.text = std::string("⚠ ") + e.what();
    state_.appendTranscriptLine(std::move(line));
  }
}

void App::openModelsMenu() {
  try {
    auto models = session_.client().listModels();
    std::vector<MenuList::Item> items;
    for (const auto& m : models.models) {
      MenuList::Item item;
      item.label = m.id;
      item.detail = m.provider;
      item.id = m.provider + ":" + m.id;
      items.push_back(std::move(item));
    }
    menu_.setTitle("Select Model");
    menu_.setItems(std::move(items));
    menu_.setOnSelect([this](const MenuList::Item& item) {
      // Parse provider:model from id.
      auto sep = item.id.find(':');
      if (sep != std::string::npos) {
        auto providerId = item.id.substr(0, sep);
        auto modelId = item.id.substr(sep + 1);
        firmius::daemon::ModelSwitchRequest req;
        req.providerId = providerId;
        req.modelId = modelId;
        session_.client().switchModel(req);
      }
      dismissMenu();
    });
    menu_.setOnDismiss([this]() { dismissMenu(); });
    state_.markDirtyPublic();
  } catch (const std::exception& e) {
    TranscriptLine line;
    line.kind = TranscriptLine::Kind::Notice;
    line.text = std::string("Failed to list models: ") + e.what();
    state_.appendTranscriptLine(std::move(line));
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
    menu_.setTitle("Resume Session");
    menu_.setItems(std::move(items));
    menu_.setOnSelect([this](const MenuList::Item& item) {
      dispatcher_.openThread(item.id);
      dismissMenu();
    });
    menu_.setOnDismiss([this]() { dismissMenu(); });
    state_.markDirtyPublic();
  } catch (const std::exception& e) {
    TranscriptLine line;
    line.kind = TranscriptLine::Kind::Notice;
    line.text = std::string("Failed to list threads: ") + e.what();
    state_.appendTranscriptLine(std::move(line));
  }
}

void App::dismissMenu() {
  menu_.setItems({});
  state_.markDirtyPublic();
}

} // namespace firmius::tui2

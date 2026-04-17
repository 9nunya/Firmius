#include "TuiRunner.hpp"
#include "AgentRegistry.hpp"
#include "TUIState.hpp"
#include "harness/Harness.hpp"
#include "workflow/WorkflowLoader.hpp"
#include <atomic>
#include <chrono>
#include <csignal>
#if !defined(_WIN32)
#include <signal.h>
#endif
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <ftxui/component/component.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/screen.hpp>
#include <iomanip>
#include <iostream>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "commands/AccountsCommand.hpp"
#include "commands/BenchmarksCommand.hpp"
#include "commands/CommandManager.hpp"
#include "commands/ConfigCommand.hpp"
#include "commands/MemoryCommand.hpp"
#include "commands/ConnectCommand.hpp"
#include "commands/McpCommand.hpp"
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
#include "modals/RollingMemorySettingsModal.hpp"
#include "modals/ModalRegistry.hpp"
#include "modals/ModelPickerModal.hpp"
#include "modals/PurposesModal.hpp"
#include "modals/RouterModal.hpp"
#include "modals/McpModal.hpp"
#include "modals/ThreadLockedModal.hpp"
#include "modals/ThreadPickerModal.hpp"

namespace {

extern "C" void HandleSigint(int) {
  g_pending_sigint.fetch_add(1, std::memory_order_relaxed);
}

void InstallSigintHandler() {
#if defined(_WIN32)
  std::signal(SIGINT, HandleSigint);
#else
  struct sigaction action{};
  action.sa_handler = HandleSigint;
  sigemptyset(&action.sa_mask);
  action.sa_flags = 0;
  sigaction(SIGINT, &action, nullptr);
#endif
}

void RestoreDefaultSigintHandler() {
  std::signal(SIGINT, SIG_DFL);
}

std::string defaultLeadPersona(const firmius::core::Harness &harness) {
  const auto &cfg = harness.getConfig();
  return cfg.defaultLeadPersona.empty() ? "lead" : cfg.defaultLeadPersona;
}

bool parseStartupProfileEnabledFromEnv() {
  const char *raw = std::getenv("FIRMIUS_TUI_STARTUP_PROFILE");
  if (!raw) {
    return false;
  }
  std::string value(raw);
  for (char &ch : value) {
    ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
  }
  if (value.empty() || value == "0" || value == "false" || value == "off" ||
      value == "no") {
    return false;
  }
  return true;
}

class StartupTimingProfiler {
 public:
  explicit StartupTimingProfiler(bool enabled) : enabled_(enabled) {
    if (!enabled_) {
      return;
    }
    start_ = Clock::now();
    cursor_ = start_;
    marks_.push_back({"run_tui_begin", 0.0});
  }

  bool enabled() const { return enabled_; }

  void completePhase(const std::string &name) {
    if (!enabled_) {
      return;
    }
    const auto now = Clock::now();
    phases_.push_back({name, toMs(cursor_), toMs(now)});
    cursor_ = now;
  }

  void mark(const std::string &name) {
    if (!enabled_) {
      return;
    }
    marks_.push_back({name, toMs(Clock::now())});
  }

  void printSummary(std::ostream &out) const {
    if (!enabled_) {
      return;
    }

    const auto oldFlags = out.flags();
    const auto oldPrecision = out.precision();
    out << std::fixed << std::setprecision(3);

    double totalMs = 0.0;
    for (const auto &phase : phases_) {
      if (phase.endMs > totalMs) {
        totalMs = phase.endMs;
      }
    }
    for (const auto &mark : marks_) {
      if (mark.second > totalMs) {
        totalMs = mark.second;
      }
    }

    out << "[tui_startup_profile] {\n";
    out << "  \"enabled\": true,\n";
    out << "  \"unit\": \"ms\",\n";
    out << "  \"total_ms\": " << totalMs << ",\n";
    out << "  \"phases\": [\n";
    for (std::size_t i = 0; i < phases_.size(); ++i) {
      const auto &phase = phases_[i];
      out << "    {\"name\": \"" << phase.name << "\", \"start_ms\": "
          << phase.startMs << ", \"end_ms\": " << phase.endMs
          << ", \"duration_ms\": " << (phase.endMs - phase.startMs) << "}";
      if (i + 1 < phases_.size()) {
        out << ",";
      }
      out << "\n";
    }
    out << "  ],\n";
    out << "  \"marks\": [\n";
    for (std::size_t i = 0; i < marks_.size(); ++i) {
      out << "    {\"name\": \"" << marks_[i].first
          << "\", \"at_ms\": " << marks_[i].second << "}";
      if (i + 1 < marks_.size()) {
        out << ",";
      }
      out << "\n";
    }
    out << "  ]\n";
    out << "}\n";

    out.flags(oldFlags);
    out.precision(oldPrecision);
  }

 private:
  using Clock = std::chrono::steady_clock;

  struct PhaseTiming {
    std::string name;
    double startMs = 0.0;
    double endMs = 0.0;
  };

  double toMs(const Clock::time_point &tp) const {
    return std::chrono::duration<double, std::milli>(tp - start_).count();
  }

  bool enabled_ = false;
  Clock::time_point start_{};
  Clock::time_point cursor_{};
  std::vector<PhaseTiming> phases_;
  std::vector<std::pair<std::string, double>> marks_;
};

} // namespace

namespace firmius::tui {

void runTui(const TuiLaunchOptions &options) {
  StartupTimingProfiler startupProfiler(parseStartupProfileEnabledFromEnv());

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
      std::make_shared<firmius::tui::MemoryCommand>());
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
      std::make_shared<firmius::tui::McpCommand>());
  firmius::tui::CommandManager::instance().registerCommand(
      std::make_shared<firmius::tui::PurposesCommand>());
  firmius::tui::CommandManager::instance().registerCommand(
      std::make_shared<firmius::tui::BenchmarksCommand>());
  // Note: /workflows command removed - workflows are now registered as
  // individual commands below
  startupProfiler.completePhase("command_registration");

  // Register Modals
  firmius::tui::ModalRegistry::instance().registerModal(
      std::make_shared<firmius::tui::ThreadPickerModal>());
  firmius::tui::ModalRegistry::instance().registerModal(
      std::make_shared<firmius::tui::ModelPickerModal>());
  firmius::tui::ModalRegistry::instance().registerModal(
      std::make_shared<firmius::tui::ConfigDisplayModal>());
  firmius::tui::ModalRegistry::instance().registerModal(
      std::make_shared<firmius::tui::RollingMemorySettingsModal>());
  firmius::tui::ModalRegistry::instance().registerModal(
      std::make_shared<firmius::tui::RouterModal>());
  firmius::tui::ModalRegistry::instance().registerModal(
      std::make_shared<firmius::tui::PurposesModal>());
  firmius::tui::ModalRegistry::instance().registerModal(
      std::make_shared<firmius::tui::McpModal>());
  startupProfiler.completePhase("modal_registration");

  auto &h = firmius::core::Harness::instance();
  h.init();
  startupProfiler.completePhase("harness_init");

  // Initialize workflow loader after harness (loads from ~/.firmius/workflows/)
  firmius::core::WorkflowLoader::instance().init();
  startupProfiler.completePhase("workflow_loader_init");

  // Register each workflow as its own command (e.g., /parallel_exploration)
  firmius::tui::registerWorkflowCommands();
  startupProfiler.completePhase("workflow_command_registration");

  auto &state = firmius::tui::TuiState::instance();

  bool thread_loaded = false;
  std::optional<std::string> startupAgentId;
  std::size_t startupBaselineTurns = 0;
  std::string threadPath = "none";
  if (!options.initialPrompt.empty()) {
    const std::string cwd = options.initialCwd.empty()
                                ? std::filesystem::current_path().string()
                                : options.initialCwd;
    const std::string threadId = h.newThread({}, cwd, defaultLeadPersona(h));
    if (!threadId.empty()) {
      h.setCurrentThreadPermissionMode(options.permissionMode);
      const std::string focusedAgentId = h.focusedAgentId();
      if (!focusedAgentId.empty()) {
        startupAgentId = focusedAgentId;
        auto agent =
            firmius::core::AgentRegistry::instance().getAgent(focusedAgentId);
        if (agent && agent->getContext().history) {
          startupBaselineTurns = agent->getContext().history->turns.size();
        }
      }
      h.send(options.initialPrompt);
      if (!startupAgentId.has_value()) {
        const std::string postSendAgentId = h.focusedAgentId();
        if (!postSendAgentId.empty()) {
          startupAgentId = postSendAgentId;
        }
      }
      thread_loaded = true;
      threadPath = "new_thread_from_initial_prompt";
    } else {
      threadPath = "new_thread_from_initial_prompt_failed";
    }
  } else if (options.debuggingMode) {
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
      threadPath = "new_thread_debugging_mode";
    } else {
      threadPath = "new_thread_debugging_mode_failed";
    }
  } else if (options.continueLast) {
    if (h.resumeLast()) {
      thread_loaded = true;
      threadPath = "resume_last_thread";
    } else {
      threadPath = "resume_last_thread_failed";
    }
  }
  startupProfiler.completePhase("thread_path_selection");
  startupProfiler.mark("thread_path:" + threadPath);

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
  startupProfiler.completePhase("tui_state_init");

  auto screen = ftxui::ScreenInteractive::Fullscreen();
  screen.TrackMouse(true);
  screen.ForceHandleCtrlC(false);
  state.attachScreen(&screen);
  InstallSigintHandler();
  const auto exitLoop = screen.ExitLoopClosure();
  startupProfiler.completePhase("screen_creation_attach");

  // Enable bracketed paste mode so terminal sends \x1b[200~ and \x1b[201~
  // sequences around pasted content, allowing us to detect multi-line pastes
  std::cout << "\x1b[?2004h" << std::flush;

  auto renderer = state.root();
  std::atomic<bool> sigint_bridge_running{true};
  std::atomic<bool> pending_idle_exit{false};
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
  std::jthread idle_exit_bridge([&](std::stop_token st) {
    if (!options.quitWhenIdle || options.initialPrompt.empty()) {
      return;
    }
    bool observedWork = false;
    while (!st.stop_requested()) {
      std::string agentId;
      if (startupAgentId.has_value() && !startupAgentId->empty()) {
        agentId = *startupAgentId;
      } else {
        agentId = h.focusedAgentId();
        if (!agentId.empty()) {
          startupAgentId = agentId;
        }
      }
      if (!agentId.empty()) {
        auto agent = firmius::core::AgentRegistry::instance().getAgent(agentId);
        if (agent) {
          const auto running = agent->isRunning() || agent->isBooting();
          const auto turnCount = agent->getContext().history
                                     ? agent->getContext().history->turns.size()
                                     : 0;
          if (running || turnCount > startupBaselineTurns) {
            observedWork = true;
          }
          if (observedWork && !running) {
            const auto status = agent->getContext().state.currentStatus;
            if (status == firmius::shared::AgentStatus::Idle ||
                status == firmius::shared::AgentStatus::Error ||
                status == firmius::shared::AgentStatus::Cancelled) {
              pending_idle_exit.store(true, std::memory_order_relaxed);
              screen.PostEvent(ftxui::Event::Custom);
              return;
            }
          }
        }
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
  });
  int handled_sigints = 0;
  renderer = CatchEvent(renderer, [&](ftxui::Event event) {
    if (pending_idle_exit.exchange(false, std::memory_order_relaxed)) {
      exitLoop();
      return true;
    }
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

  std::atomic<bool> firstInteractivePaintObserved{false};
  if (startupProfiler.enabled()) {
    auto observedRenderer = renderer;
    renderer = ftxui::Renderer(observedRenderer,
                               [&startupProfiler, &firstInteractivePaintObserved,
                                observedRenderer] {
                                 if (!firstInteractivePaintObserved.exchange(
                                         true, std::memory_order_relaxed)) {
                                   startupProfiler.mark(
                                       "first_interactive_paint_boundary");
                                 }
                                 return observedRenderer->Render();
                               });
  }

  startupProfiler.mark("loop_entry");
  screen.Loop(renderer);
  startupProfiler.mark("loop_exit");
  startupProfiler.completePhase("event_loop_runtime");

  sigint_bridge_running = false;
  sigint_bridge.request_stop();
  idle_exit_bridge.request_stop();
  RestoreDefaultSigintHandler();
  std::cout << "\x1b[?2004l" << std::flush;
  startupProfiler.completePhase("shutdown_bridges_and_terminal");

  const std::string exit_summary = state.exitSummaryText();
  state.shutdown();
  startupProfiler.completePhase("shutdown_tui_state");
  h.shutdown();
  startupProfiler.completePhase("shutdown_harness");

  startupProfiler.printSummary(std::cerr);
  if (startupProfiler.enabled()) {
    std::cerr << exit_summary << std::flush;
  }
  std::cout << exit_summary << std::flush;
}

} // namespace firmius::tui

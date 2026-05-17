#include "TuiRunner.hpp"
#include "AgentRegistry.hpp"
#include "FatalSignalHandler.hpp"
#include "NotificationManager.hpp"
#include "TUIState.hpp"
#include "agents/PurposeLoader.hpp"
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
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "commands/AccountsCommand.hpp"
#include "commands/BenchmarksCommand.hpp"
#include "commands/CommandManager.hpp"
#include "commands/ConfigCommand.hpp"
#include "commands/ConnectCommand.hpp"
#include "commands/EditHistoryCommand.hpp"
#include "commands/HistoryCommand.hpp"
#include "commands/HooksCommand.hpp"
#include "commands/UndoRedoCommands.hpp"
#include "commands/McpCommand.hpp"
#include "commands/ModeCommand.hpp"
#include "commands/ModelCommand.hpp"
#include "commands/NewCommand.hpp"
#include "commands/PurposesCommand.hpp"
#include "commands/QuitCommand.hpp"
#include "commands/QuotasCommand.hpp"
#include "commands/RouterCommand.hpp"
#include "commands/ProvidersCommand.hpp"
#include "commands/SkinCommand.hpp"
#include "commands/ThreadsCommand.hpp"
#include "commands/UndoCommand.hpp"
#include "commands/WorkflowsCommand.hpp"
#include "modals/CommandPaletteModal.hpp"
#include "modals/ConfigDisplayModal.hpp"
#include "modals/KeybindingEditorModal.hpp"
#include "modals/HooksModal.hpp"
#include "modals/McpModal.hpp"
#include "modals/ModalRegistry.hpp"
#include "modals/ModelPickerModal.hpp"
#include "modals/PurposesModal.hpp"
#include "modals/ProvidersModal.hpp"
#include "modals/RouterModal.hpp"
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

std::string defaultLeadPersona(const firmius::core::Harness &harness) {
  const auto &cfg = harness.getConfig();
  if (!cfg.defaultLeadPersona.empty() &&
      firmius::core::PurposeLoader::isValid(cfg.defaultLeadPersona)) {
    return cfg.defaultLeadPersona;
  }
  if (firmius::core::PurposeLoader::isValid("lead")) {
    return "lead";
  }
  const auto purposes = firmius::core::PurposeLoader::listPurposes();
  return purposes.empty() ? "lead" : purposes.front();
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

std::chrono::milliseconds parseTuiWatchdogThresholdFromEnv() {
  const char *raw = std::getenv("FIRMIUS_TUI_WATCHDOG_MS");
  if (!raw || *raw == '\0') {
    return std::chrono::milliseconds(0);
  }
  try {
    const long long value = std::stoll(raw);
    if (value <= 0) {
      return std::chrono::milliseconds(0);
    }
    return std::chrono::milliseconds(value);
  } catch (...) {
    return std::chrono::milliseconds(0);
  }
}

class StartupTimingProfiler {
public:
  explicit StartupTimingProfiler(bool printSummary) : print_summary_(printSummary) {
    start_ = Clock::now();
    cursor_ = start_;
    marks_.push_back({"run_tui_begin", 0.0});
  }

  bool enabled() const { return print_summary_; }

  double totalMs() const { return computeTotalMs(); }

  double elapsedMs() const {
    return std::chrono::duration<double, std::milli>(Clock::now() - start_)
        .count();
  }

  void completePhase(const std::string &name) {
    const auto now = Clock::now();
    phases_.push_back({name, toMs(cursor_), toMs(now)});
    cursor_ = now;
  }

  void mark(const std::string &name) {
    marks_.push_back({name, toMs(Clock::now())});
  }

  void printSummary(std::ostream &out) const {
    if (!print_summary_) {
      return;
    }

    const auto oldFlags = out.flags();
    const auto oldPrecision = out.precision();
    out << std::fixed << std::setprecision(3);

    const double totalMs = computeTotalMs();

    out << "[tui_startup_profile] {\n";
    out << "  \"enabled\": true,\n";
    out << "  \"unit\": \"ms\",\n";
    out << "  \"total_ms\": " << totalMs << ",\n";
    out << "  \"phases\": [\n";
    for (std::size_t i = 0; i < phases_.size(); ++i) {
      const auto &phase = phases_[i];
      out << "    {\"name\": \"" << phase.name
          << "\", \"start_ms\": " << phase.startMs
          << ", \"end_ms\": " << phase.endMs
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

  double computeTotalMs() const {
    double totalMs = 0.0;
    for (const auto &phase : phases_) {
      totalMs = std::max(totalMs, phase.endMs);
    }
    for (const auto &mark : marks_) {
      totalMs = std::max(totalMs, mark.second);
    }
    return totalMs;
  }

  bool print_summary_ = false;
  Clock::time_point start_{};
  Clock::time_point cursor_{};
  std::vector<PhaseTiming> phases_;
  std::vector<std::pair<std::string, double>> marks_;
};

struct StartupVisualPhase {
  std::string phase_key;
  std::string title;
  std::string detail;
};

const std::vector<StartupVisualPhase> &startupVisualPhases() {
  static const std::vector<StartupVisualPhase> phases = {
      {"command_registration", "Loading command registry",
       "Registering built-in slash commands: new, threads, model, undo, config, history, memory, mode, providers, and benchmarks."},
      {"modal_registration", "Loading modal registry",
       "Registering thread picker, model picker, command palette, config, keybindings, router, providers, purposes, and MCP modals."},
      {"harness_init", "Loading runtime harness",
       "Initializing thread locks, persisted session metadata, config, providers, permissions, and runtime event routing."},
      {"workflow_loader_init", "Loading workflow definitions",
       "Scanning installed workflow files from the configured Firmius workflow directories."},
      {"workflow_command_registration", "Loading workflow commands",
       "Publishing discovered workflows as slash-command entrypoints."},
      {"thread_path_selection", "Loading startup thread path",
       "Resolving whether to show Welcome, restore a selected thread, continue the last thread, or launch an initial prompt."},
      {"tui_state_init", "Loading first-frame TUI state",
       "Hydrating store models, welcome/chat view state, focused transcript cache, and status bar model."},
      {"screen_creation_attach", "Loading terminal screen",
       "Creating the fullscreen FTXUI screen, installing input handlers, and binding the first render tree."},
  };
  return phases;
}

std::string formatStartupSummaryLine(const StartupTimingProfiler &profiler) {
  std::ostringstream out;
  const double totalMs = profiler.totalMs();
  if (totalMs < 1000.0) {
    out << "Loaded in " << std::fixed << std::setprecision(1) << totalMs
        << " ms.";
  } else {
    out << "Loaded in " << std::fixed << std::setprecision(2)
        << (totalMs / 1000.0) << " s (" << std::setprecision(0) << totalMs
        << " ms).";
  }
  return out.str();
}

void applyStartupVisualPhase(firmius::tui::TuiState &state,
                             const StartupTimingProfiler &profiler,
                             std::size_t phase_index, bool completed) {
  const auto &phases = startupVisualPhases();
  if (phase_index >= phases.size()) {
    return;
  }

  const auto &phase = phases[phase_index];
  const float numerator = static_cast<float>(completed ? (phase_index + 1)
                                                       : phase_index);
  const float denominator =
      static_cast<float>(std::max<std::size_t>(1, phases.size()));
  state.setLoadingMessage(phase.title);
  std::ostringstream detail;
  detail << "Loading " << phase.phase_key << ": " << phase.detail
         << " Step " << (phase_index + 1) << " of " << phases.size()
         << (completed ? " complete." : " running.") << " Elapsed "
         << std::fixed << std::setprecision(1) << profiler.elapsedMs()
         << " ms.";
  state.setLoadingDetail(detail.str());
  state.setLoadingProgress(numerator / denominator);

  if (completed && phase_index + 1 == phases.size()) {
    state.setLoadingMessage("Startup complete.");
    state.setLoadingDetail(formatStartupSummaryLine(profiler));
    state.setLoadingProgress(1.0f);
  }
}

} // namespace

namespace firmius::tui {

void runTui(const TuiLaunchOptions &options) {
  StartupTimingProfiler startupProfiler(parseStartupProfileEnabledFromEnv());
  auto &state = firmius::tui::TuiState::instance();

  state.setLoadingMessage("Starting Firmius…");
  state.setLoadingDetail(
      "Loading startup_bootstrap: preparing the first frame, timing collector, command registry, and runtime state. Step 0 of 8 running. Elapsed 0.0 ms.");
  state.setLoadingProgress(0.0f);

  auto completeStartupPhase = [&](std::size_t phase_index,
                                  const std::string &phase_name) {
    startupProfiler.completePhase(phase_name);
    applyStartupVisualPhase(state, startupProfiler, phase_index, true);
  };

  // Register Commands
  applyStartupVisualPhase(state, startupProfiler, 0, false);
  firmius::tui::CommandManager::instance().registerCommand(
      std::make_shared<firmius::tui::NewCommand>());
  firmius::tui::CommandManager::instance().registerCommand(
      std::make_shared<firmius::tui::ThreadsCommand>());
  firmius::tui::CommandManager::instance().registerCommand(
      std::make_shared<firmius::tui::ModelCommand>());
  firmius::tui::CommandManager::instance().registerCommand(
      std::make_shared<firmius::tui::UndoCommand>());
  firmius::tui::CommandManager::instance().registerCommand(
      std::make_shared<firmius::tui::UndoTurnCommand>());
  firmius::tui::CommandManager::instance().registerCommand(
      std::make_shared<firmius::tui::RedoCommand>());
  firmius::tui::CommandManager::instance().registerCommand(
      std::make_shared<firmius::tui::ConfigCommand>());
  firmius::tui::CommandManager::instance().registerCommand(
      std::make_shared<firmius::tui::HistoryCommand>());
  firmius::tui::CommandManager::instance().registerCommand(
      std::make_shared<firmius::tui::EditsCommand>());
  firmius::tui::CommandManager::instance().registerCommand(
      std::make_shared<firmius::tui::UndoEditCommand>());
  firmius::tui::CommandManager::instance().registerCommand(
      std::make_shared<firmius::tui::RedoEditCommand>());
  firmius::tui::CommandManager::instance().registerCommand(
      std::make_shared<firmius::tui::UndoTranscriptCommand>());
  firmius::tui::CommandManager::instance().registerCommand(
      std::make_shared<firmius::tui::RedoTranscriptCommand>());
  firmius::tui::CommandManager::instance().registerCommand(
      std::make_shared<firmius::tui::ModeCommand>());
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
      std::make_shared<firmius::tui::ProvidersCommand>());
  firmius::tui::CommandManager::instance().registerCommand(
      std::make_shared<firmius::tui::McpCommand>());
  firmius::tui::CommandManager::instance().registerCommand(
      std::make_shared<firmius::tui::HooksCommand>());
  firmius::tui::CommandManager::instance().registerCommand(
      std::make_shared<firmius::tui::PurposesCommand>());
  firmius::tui::CommandManager::instance().registerCommand(
      std::make_shared<firmius::tui::BenchmarksCommand>());
  firmius::tui::CommandManager::instance().registerCommand(
      std::make_shared<firmius::tui::SkinCommand>());
  // Note: /workflows command removed - workflows are now registered as
  // individual commands below
  completeStartupPhase(0, "command_registration");

  // Register Modals
  applyStartupVisualPhase(state, startupProfiler, 1, false);
  firmius::tui::ModalRegistry::instance().registerModal(
      std::make_shared<firmius::tui::ThreadPickerModal>());
  firmius::tui::ModalRegistry::instance().registerModal(
      std::make_shared<firmius::tui::ModelPickerModal>());
  firmius::tui::ModalRegistry::instance().registerModal(
      std::make_shared<firmius::tui::CommandPaletteModal>());
  firmius::tui::ModalRegistry::instance().registerModal(
      std::make_shared<firmius::tui::ConfigDisplayModal>());
  firmius::tui::ModalRegistry::instance().registerModal(
      std::make_shared<firmius::tui::KeybindingEditorModal>());
  firmius::tui::ModalRegistry::instance().registerModal(
      std::make_shared<firmius::tui::RouterModal>());
  firmius::tui::ModalRegistry::instance().registerModal(
      std::make_shared<firmius::tui::ProvidersModal>());
  firmius::tui::ModalRegistry::instance().registerModal(
      std::make_shared<firmius::tui::PurposesModal>());
  firmius::tui::ModalRegistry::instance().registerModal(
      std::make_shared<firmius::tui::McpModal>());
  firmius::tui::ModalRegistry::instance().registerModal(
      std::make_shared<firmius::tui::HooksModal>());
  completeStartupPhase(1, "modal_registration");

  auto &h = firmius::core::Harness::instance();
  applyStartupVisualPhase(state, startupProfiler, 2, false);
  h.init();
  completeStartupPhase(2, "harness_init");

  // Initialize workflow loader after harness (loads from ~/.firmius/workflows/)
  applyStartupVisualPhase(state, startupProfiler, 3, false);
  firmius::core::WorkflowLoader::instance().init();
  completeStartupPhase(3, "workflow_loader_init");

  // Register each workflow as its own command (e.g., /parallel_exploration)
  applyStartupVisualPhase(state, startupProfiler, 4, false);
  firmius::tui::registerWorkflowCommands();
  completeStartupPhase(4, "workflow_command_registration");

  bool thread_loaded = false;
  std::optional<std::string> startupAgentId;
  std::optional<std::string> deferredThreadSwitchId;
  bool deferredResumeLast = false;
  std::size_t startupBaselineTurns = 0;
  firmius::shared::ThreadMetadata current_metadata;
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
  } else if (!options.threadId.empty()) {
    deferredThreadSwitchId = options.threadId;
    threadPath = "deferred_switch_thread:" + options.threadId;
  } else if (options.continueLast) {
    thread_loaded = true;
    threadPath = "continue_last_already_restored";
    deferredResumeLast = true;
  }
  completeStartupPhase(5, "thread_path_selection");
  startupProfiler.mark("thread_path:" + threadPath);

  if (thread_loaded) {
    const std::string current_id = h.currentThreadId();
    // Fast-path single-thread lookup; the prior listThreads() scan cost
    // O(num_threads) at startup for every resumed session.
    auto resumed_meta = h.getThreadMetadata(current_id);
    if (resumed_meta.threadId == current_id) {
      current_metadata = resumed_meta;
      h.setFocusedAgent(h.focusedAgentId());
    }
    state.init(h, current_metadata, h.focusedAgentId());
    state.setViewMode(firmius::tui::TuiState::ViewMode::Chat);
  } else {
    firmius::shared::ThreadMetadata dummy_thread;
    state.init(h, dummy_thread, "");
    state.setViewMode(firmius::tui::TuiState::ViewMode::Welcome);
  }
  completeStartupPhase(6, "tui_state_init");

  auto screen = ftxui::ScreenInteractive::Fullscreen();
  screen.TrackMouse(true);
  screen.ForceHandleCtrlC(false);
  state.attachScreen(&screen);
  installFatalSignalHandlers();
  InstallSigintHandler();
  const auto exitLoop = screen.ExitLoopClosure();
  completeStartupPhase(7, "screen_creation_attach");
  // Start autonomous animation tick for heartbeats, glints, starfields.
  // TuiState handles joining this thread in its shutdown() method.
  state.attachScreen(&screen);

  // Enable bracketed paste mode so terminal sends \x1b[200~ and \x1b[201~
  // sequences around pasted content, allowing us to detect multi-line pastes
  std::cout << "\x1b[?2004h" << std::flush;

  auto renderer = state.root();
  if (deferredThreadSwitchId.has_value() || deferredResumeLast) {
    state.requestThreadOpen(
        deferredThreadSwitchId, deferredResumeLast,
        deferredResumeLast ? "Restoring last thread..." : "Opening thread...",
        deferredResumeLast ? "Loading the last active thread from disk."
                           : "Loading the selected thread from disk.");
  }
  // Avoid replaying ThreadChanged here: init() already loaded the focused
  // transcript, and a synthetic replay would clear any live stream state that
  // resumed agents emit before the user presses another key.
  std::atomic<bool> sigint_bridge_running{true};
  std::atomic<bool> pending_idle_exit{false};
  std::jthread sigint_bridge([&](std::stop_token st) {
    int last_seen = 0;
    while (!st.stop_requested() && sigint_bridge_running.load()) {
      InstallSigintHandler();
      const int current = g_pending_sigint.load(std::memory_order_relaxed);
      if (current != last_seen) {
        last_seen = current;
        screen.PostEvent(ftxui::Event::CtrlC);
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
  std::jthread fatal_signal_refresh([&](std::stop_token st) {
    // FTXUI installs its own SIGSEGV/SIGBUS handlers during Loop startup.
    // Re-apply Firmius' fatal handler for a short window so render faults
    // crash cleanly instead of re-executing the same bad instruction forever.
    for (int attempt = 0; attempt < 20 && !st.stop_requested(); ++attempt) {
      installFatalSignalHandlers();
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
  });
  const auto watchdog_threshold = parseTuiWatchdogThresholdFromEnv();
  std::jthread ui_watchdog([&](std::stop_token st) {
    if (watchdog_threshold.count() <= 0) {
      return;
    }
    const auto poll_interval =
        std::max(std::chrono::milliseconds(50), watchdog_threshold / 4);
    while (!st.stop_requested()) {
      std::this_thread::sleep_for(poll_interval);
      const auto last_frame_ms = firmius::tui::tuiLastFrameRenderedAtMs();
      if (last_frame_ms == 0) {
        continue;
      }
      const auto now_ms = static_cast<std::uint64_t>(
          std::chrono::duration_cast<std::chrono::milliseconds>(
              std::chrono::steady_clock::now().time_since_epoch())
              .count());
      if (now_ms <= last_frame_ms) {
        continue;
      }
      const auto age = std::chrono::milliseconds(now_ms - last_frame_ms);
      if (age <= watchdog_threshold) {
        continue;
      }

      const bool should_enforce =
          !state.loadingMessage().empty() || state.statusText() != "idle";
      if (!should_enforce) {
        continue;
      }

      std::cerr << "\n[FIRMIUS TUI WATCHDOG] frame stalled for "
                << age.count() << " ms while UI was active.\n";
      std::raise(SIGABRT);
      return;
    }
  });
  renderer = CatchEvent(renderer, [&](ftxui::Event) {
    if (pending_idle_exit.exchange(false, std::memory_order_relaxed)) {
      exitLoop();
      return true;
    }
    return false;
  });

  std::thread([&state]() {
    std::this_thread::sleep_for(std::chrono::milliseconds(900));
    if (state.loadingMessage() == "Startup complete.") {
      state.clearLoadingState();
    }
  }).detach();

  screen.Loop(renderer);

  // Disable bracketed paste mode on exit to restore terminal state.
  std::cout << "\x1b[?2004l" << std::flush;

  state.shutdown();
  // Harness owns a std::vector<std::thread> for background tasks (title
  // generation, model discovery, subagent reap helpers). If we don't join
  // them here, ~Harness runs on process exit via static singleton teardown
  // with those threads still joinable → std::terminate("called without an
  // active exception") → Aborted core dump right before the exit summary
  // can print. Harness::shutdown() moves the vector out and joins each.
  firmius::core::Harness::instance().shutdown();
  sigint_bridge_running.store(false);
  startupProfiler.completePhase("shutdown_cleanup");
  state.clearLoadingState();

  if (startupProfiler.enabled()) {
    startupProfiler.printSummary(std::cerr);
  }
}

} // namespace firmius::tui

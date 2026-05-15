#include "CliOptions.hpp"
#include "TuiRunner.hpp"
#include "agents/hooks/HookRegistry.hpp"
#include "agents/hooks/HookState.hpp"
#include "daemon/DaemonClient.hpp"
#include "Panic.hpp"
#include "workflow/WorkflowLoader.hpp"
#include <chrono>
#include <filesystem>
#include <exception>
#include <iostream>
#include <string>
#include <thread>

#if defined(_WIN32)
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace {

int runHooksCli(int argc, char **argv) {
  firmius::core::WorkflowLoader::instance().init();
  firmius::core::hooks::HookRegistry::instance().reload();

  const std::string action = argc > 2 ? argv[2] : "list";
  if (action == "reload") {
    std::cout << "Hooks reloaded. Registered event hooks: "
              << firmius::core::hooks::HookRegistry::instance().size() << "\n";
    return 0;
  }

  if (action == "state") {
    if (argc < 4) {
      std::cerr << "usage: firmius hooks state <thread-id> [path]\n";
      return 1;
    }
    auto &state = firmius::core::hooks::HookState::instance();
    state.bindThread(argv[3]);
    if (argc >= 5) {
      auto value = state.readJson(firmius::core::hooks::HookState::Scope::Thread,
                                  argv[4], "cli.hooks");
      std::cout << (value.has_value() ? *value : "null") << "\n";
    } else {
      std::cout << state.snapshotJson("cli.hooks") << "\n";
    }
    return 0;
  }

  const auto workflows =
      firmius::core::WorkflowLoader::instance().getAllWorkflows();
  std::cout << "event hooks: "
            << firmius::core::hooks::HookRegistry::instance().size() << "\n";
  for (const auto &workflow : workflows) {
    if (workflow.isHook()) {
      std::cout << "hook " << workflow.id << " event="
                << firmius::core::workflowEventKindToString(
                       workflow.trigger.event)
                << " action="
                << firmius::core::workflowActionKindToString(
                       workflow.action.kind)
                << " source=" << workflow.sourcePath << "\n";
    } else if (workflow.slashCommand.has_value()) {
      std::cout << "command " << *workflow.slashCommand << " id="
                << workflow.id << " action="
                << firmius::core::workflowActionKindToString(
                       workflow.action.kind)
                << " source=" << workflow.sourcePath << "\n";
    }
  }
  return 0;
}

int currentPid() {
#if defined(_WIN32)
  return static_cast<int>(GetCurrentProcessId());
#else
  return static_cast<int>(::getpid());
#endif
}

std::string siblingDaemonPath(const std::filesystem::path &argv0) {
#if defined(_WIN32)
  const auto candidate = argv0.parent_path() / "firmiusd.exe";
#else
  const auto candidate = argv0.parent_path() / "firmiusd";
#endif
  if (std::filesystem::exists(candidate)) {
    return candidate.string();
  }
  return {};
}

int runDaemonSmokeCli(int argc, char **argv) {
  std::string endpoint = firmius::daemon::kDefaultEndpoint;
  std::string prompt;
  std::string threadId;
  std::string cwd = std::filesystem::current_path().string();
  bool autoStart = true;

  for (int i = 2; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--endpoint" && i + 1 < argc) {
      endpoint = argv[++i];
      continue;
    }
    if (arg == "--prompt" && i + 1 < argc) {
      prompt = argv[++i];
      continue;
    }
    if (arg == "--thread-id" && i + 1 < argc) {
      threadId = argv[++i];
      continue;
    }
    if (arg == "--cwd" && i + 1 < argc) {
      cwd = argv[++i];
      continue;
    }
    if (arg == "--no-autostart") {
      autoStart = false;
      continue;
    }
    if (arg == "--help" || arg == "-h") {
      std::cout
          << "usage: firmius daemon-smoke [--endpoint PATH] [--thread-id ID]"
          << " [--prompt TEXT] [--cwd PATH] [--no-autostart]\n";
      return 0;
    }
    std::cerr << "firmius daemon-smoke unknown argument: " << arg << "\n";
    return 2;
  }

  firmius::daemon::DaemonClientOptions options;
  options.connection.endpoint = endpoint;
  options.autoStart = autoStart;
  options.subscribeToEvents = true;
  options.daemonExecutablePath = siblingDaemonPath(std::filesystem::path(argv[0]));
  options.identity.clientId = "smoke-" +
                              std::to_string(currentPid()) + "-" +
                              std::to_string(std::chrono::steady_clock::now()
                                                 .time_since_epoch()
                                                 .count());
  options.identity.uiKind = "smoke";
  options.identity.pid = currentPid();
  options.identity.capabilityFlags = {"rpc", "events"};
  options.presence.cwd = cwd;
  options.presence.workspaceRoot = cwd;
  options.presence.repoRoot = cwd;

  firmius::daemon::DaemonClient client(options);
  if (!client.connect()) {
    std::cerr << "firmius daemon-smoke failed to connect to daemon\n";
    return 1;
  }

  const auto ping = client.ping();
  std::cout << "connected protocol=" << ping.protocolVersion << " pid=" << ping.pid
            << "\n";

  if (!client.subscribe([](const firmius::daemon::DaemonEventEnvelope &event) {
        std::cout << "event kind=" << static_cast<int>(event.kind);
        if (!event.runtimeEventType.empty()) {
          std::cout << " type=" << event.runtimeEventType;
        }
        if (!event.runtimeEventThreadId.empty()) {
          std::cout << " thread=" << event.runtimeEventThreadId;
        }
        if (!event.runtimeEventAgentId.empty()) {
          std::cout << " agent=" << event.runtimeEventAgentId;
        }
        if (event.session.has_value()) {
          std::cout << " client=" << event.session->identity.clientId;
        }
        std::cout << "\n";
      })) {
    std::cerr << "firmius daemon-smoke failed to subscribe to daemon events\n";
    return 1;
  }

  if (threadId.empty()) {
    const auto created = client.createThread(
        firmius::daemon::ThreadsCreateRequest{cwd, "lead", "",
                                              firmius::shared::ThreadPermissionMode::Request});
    threadId = created.thread.threadId;
    std::cout << "thread created " << threadId << "\n";
  } else {
    const auto opened = client.openThread(threadId);
    if (!opened.opened) {
      std::cerr << "firmius daemon-smoke failed to open thread " << threadId << "\n";
      return 1;
    }
    std::cout << "thread opened " << threadId << "\n";
  }

  if (!prompt.empty()) {
    const auto response =
        client.send(firmius::daemon::ThreadsSendRequest{threadId, "", prompt, {}});
    if (!response.accepted) {
      std::cerr << "firmius daemon-smoke send rejected\n";
      return 1;
    }
    std::cout << "prompt sent to " << response.threadId << "\n";
    std::this_thread::sleep_for(std::chrono::seconds(2));
  }

  client.disconnect();
  return 0;
}

} // namespace

int main(int argc, char **argv) {
  firmius::shared::Panic::init();

  if (argc > 1) {
    const std::string first = argv[1];
    if (first == "--help" || first == "-h") {
      std::cout
          << "usage: firmius [options]\n\n"
          << "Options:\n"
          << "  -h, --help                 Show this help\n"
          << "  -V, --version              Show version\n"
          << "  -c                         Continue last thread\n"
          << "  --thread-id <id>            Continue a specific thread\n"
          << "  -p, --prompt <text>         Send initial prompt\n"
          << "  -P, --prompt-file <path>    Send initial prompt from file\n"
          << "  -C, --cwd <path>            Initial working directory\n"
          << "  --quit-when-idle            Quit when idle\n"
          << "  --permission-mode <mode>    request|always-allow|deny-all\n"
          << "\nSubcommands:\n"
          << "  daemon-smoke                Exercise daemon IPC without TUI\n"
          << "  hooks [list|reload|state]   Manage hooks\n";
      return 0;
    }
    if (first == "--version" || first == "-V") {
      std::cout << "firmius (tui)\n";
      return 0;
    }
    if (first == "daemon-smoke") {
      return runDaemonSmokeCli(argc, argv);
    }
    if (first == "hooks") {
      return runHooksCli(argc, argv);
    }
  }

  firmius::cli::CliOptions cliOptions;
  try {
    cliOptions = firmius::cli::parseCliOptions(argc, argv);
  } catch (const std::exception &e) {
    std::cerr << e.what() << std::endl;
    return 1;
  }

  if (cliOptions.showHelp) {
    std::cout << "usage: firmius [options] (run with --help for details)\n";
    return 0;
  }
  if (cliOptions.showVersion) {
    std::cout << "firmius (tui)\n";
    return 0;
  }

  firmius::tui::TuiLaunchOptions options;
  options.debuggingMode = cliOptions.debuggingMode;
  options.continueLast = cliOptions.continueLast;
  options.initialPrompt = cliOptions.initialPrompt;
  options.initialCwd = cliOptions.initialCwd;
  options.quitWhenIdle = cliOptions.quitWhenIdle;
  options.permissionMode = cliOptions.permissionMode;
  options.threadId = cliOptions.threadId;

  firmius::tui::runTui(options);

  return 0;
}

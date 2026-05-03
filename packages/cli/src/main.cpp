#include "CliOptions.hpp"
#include "TuiRunner.hpp"
#include "agents/hooks/HookRegistry.hpp"
#include "agents/hooks/HookState.hpp"
#include "workflow/WorkflowLoader.hpp"
#include <exception>
#include <iostream>
#include <string>

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

} // namespace

int main(int argc, char **argv) {
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
          << "  hooks [list|reload|state]   Manage hooks\n";
      return 0;
    }
    if (first == "--version" || first == "-V") {
      std::cout << "firmius (tui)\n";
      return 0;
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

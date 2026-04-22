#include "CliOptions.hpp"
#include "TuiRunner.hpp"
#include <exception>
#include <iostream>
#include <string>

int main(int argc, char **argv) {
  firmius::cli::CliOptions cliOptions;
  try {
    cliOptions = firmius::cli::parseCliOptions(argc, argv);
  } catch (const std::exception &e) {
    std::cerr << e.what() << std::endl;
    return 1;
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

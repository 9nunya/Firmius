#include "CliOptions.hpp"
#include "TuiRunner.hpp"
#include "WebRunner.hpp"
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

  if (!cliOptions.web) {
    firmius::tui::TuiLaunchOptions options;
    options.debuggingMode = cliOptions.debuggingMode;
    options.continueLast = cliOptions.continueLast;
    options.initialPrompt = cliOptions.initialPrompt;
    options.initialCwd = cliOptions.initialCwd;
    options.quitWhenIdle = cliOptions.quitWhenIdle;
    options.permissionMode = cliOptions.permissionMode;
    firmius::tui::runTui(options);
  } else {
    firmius::web::runWeb(cliOptions.webHostname, cliOptions.webPort);
  }

  return 0;
}

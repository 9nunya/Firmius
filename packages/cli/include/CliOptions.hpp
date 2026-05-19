#ifndef FIRMIUS_CLI_CLIOPTIONS_HPP
#define FIRMIUS_CLI_CLIOPTIONS_HPP

#include "Enums.hpp"
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>

namespace firmius::cli {

struct CliOptions {
  enum class Mode { Tui };
  Mode mode = Mode::Tui;
  bool continueLast = false;
  bool debuggingMode = false;
  bool quitWhenIdle = false;
  bool showHelp = false;
  bool showVersion = false;
  firmius::shared::ThreadPermissionMode permissionMode =
      firmius::shared::ThreadPermissionMode::Request;
  std::string initialPrompt;
  std::string initialCwd;
  std::string threadId;
};

inline firmius::shared::ThreadPermissionMode parsePermissionMode(
    const std::string &value) {
  if (value == "request" || value == "ask") {
    return firmius::shared::ThreadPermissionMode::Request;
  }
  if (value == "always-allow" || value == "always_allow" || value == "allow") {
    return firmius::shared::ThreadPermissionMode::AlwaysAllow;
  }
  if (value == "deny-all" || value == "deny_all" || value == "deny") {
    return firmius::shared::ThreadPermissionMode::DenyAll;
  }
  throw std::runtime_error("Unknown permission mode: " + value);
}

inline std::string requireOptionValue(int argc, char **argv, int &index,
                                      const std::string &option) {
  if (index + 1 >= argc) {
    throw std::runtime_error("Missing value for " + option);
  }
  return argv[++index];
}

inline std::string readPromptFile(const std::string &path) {
  std::ifstream input(path);
  if (!input.is_open()) {
    throw std::runtime_error("Failed to open prompt file: " + path);
  }

  std::ostringstream buffer;
  buffer << input.rdbuf();
  return buffer.str();
}

inline CliOptions parseCliOptions(int argc, char **argv) {
  CliOptions options;

  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];

    if (arg == "-c") {
      options.continueLast = true;
    } else if (arg == "--thread-id") {
      options.threadId = requireOptionValue(argc, argv, i, arg);
      options.continueLast = true;
    } else if (arg == "--i-am-debugging") {
      options.debuggingMode = true;
    } else if (arg == "--prompt" || arg == "-p") {
      options.initialPrompt = requireOptionValue(argc, argv, i, arg);
    } else if (arg == "--prompt-file" || arg == "-P") {
      options.initialPrompt =
          readPromptFile(requireOptionValue(argc, argv, i, arg));
    } else if (arg == "--cwd" || arg == "-C") {
      options.initialCwd = requireOptionValue(argc, argv, i, arg);
    } else if (arg == "--quit-when-idle") {
      options.quitWhenIdle = true;
    } else if (arg == "--permission-mode") {
      options.permissionMode = parsePermissionMode(
          requireOptionValue(argc, argv, i, arg));
    } else if (arg == "--help" || arg == "-h") {
      options.showHelp = true;
    } else if (arg == "--version" || arg == "-V") {
      options.showVersion = true;
    }
  }

  return options;
}

} // namespace firmius::cli

#endif // FIRMIUS_CLI_CLIOPTIONS_HPP

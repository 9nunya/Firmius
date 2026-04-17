#pragma once

#include "Enums.hpp"
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>

namespace firmius::cli {

struct CliOptions {
  bool continueLast = false;
  bool debuggingMode = false;
  bool quitWhenIdle = false;
  firmius::shared::ThreadPermissionMode permissionMode =
      firmius::shared::ThreadPermissionMode::Request;
  std::string initialPrompt;
  std::string initialCwd;
};

inline firmius::shared::ThreadPermissionMode parsePermissionMode(
    const std::string &value) {
  if (value == "request") {
    return firmius::shared::ThreadPermissionMode::Request;
  }
  if (value == "always-allow" || value == "allow") {
    return firmius::shared::ThreadPermissionMode::AlwaysAllow;
  }
  if (value == "deny-all" || value == "deny") {
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
    }
  }

  return options;
}

} // namespace firmius::cli

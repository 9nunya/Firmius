#pragma once

#include "TUIState.hpp"
#include <string>
#include <vector>

namespace firmius::tui {

enum class ArgType { String, Number, AgentId, ThreadId, Filepath, Provider, OAuthProvider, ProviderId };

struct CommandArg {
  std::string name;
  ArgType type;
  std::string description;
  bool optional = false;
};

struct ParsedArg {
  ArgType type;
  std::string raw_value;

  double asNumber() const { return std::stod(raw_value); }
  int asInt() const { return std::stoi(raw_value); }
  std::string asString() const { return raw_value; }
};

struct CommandCtx {
  TuiState *state;
};

class ICommand {
public:
  virtual ~ICommand() = default;

  virtual std::string name() const = 0;
  virtual std::string description() const = 0;

  // Ordered list of expected arguments
  virtual std::vector<CommandArg> args() const = 0;

  // Ordered list of parsed arguments mapped 1:1 with args()
  virtual void execute(CommandCtx &ctx,
                       const std::vector<ParsedArg> &parsed_args) = 0;

  // Returns true if this command creates/uses an agent thread (e.g., workflows)
  // Returns false for utility commands (e.g., /threads, /help, /config)
  virtual bool isWorkflow() const { return false; }
};

} // namespace firmius::tui

#pragma once

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace firmius::tui2 {

// ── Typed argument system ──

enum class ArgType {
  String,
  Number,
  ThreadId,
  AgentId,
  Filepath,
  ProviderId,
  Mode
};

struct CommandArg {
  std::string name;
  ArgType type = ArgType::String;
  std::string description;
  bool optional = false;
};

struct ParsedArg {
  ArgType type = ArgType::String;
  std::string rawValue;

  double asNumber() const { return std::stod(rawValue); }
  int asInt() const { return std::stoi(rawValue); }
  const std::string& asString() const { return rawValue; }
};

// ── Command interface ──

class ICommand {
public:
  virtual ~ICommand() = default;

  virtual std::string name() const = 0;
  virtual std::string description() const = 0;
  virtual std::vector<CommandArg> args() const { return {}; }
  virtual void execute(const std::vector<ParsedArg>& args) = 0;

  /// If true, everything after the command name is passed as one raw arg.
  virtual bool takesRawRemainder() const { return false; }

  /// If true, this command was autoloaded from a workflow definition.
  virtual bool isWorkflow() const { return false; }
};

// ── Autocomplete ──

struct AutocompleteMatch {
  std::string name;
  std::string description;
  bool isExact = false;
};

// ── Command Manager ──

class CommandManager {
public:
  CommandManager() = default;

  void registerCommand(std::shared_ptr<ICommand> command);

  /// Parse and execute a "/" prefixed input string.
  /// Returns true if a command was matched and executed.
  bool execute(const std::string& input);

  /// Get autocomplete matches for a partial input (after the "/").
  std::vector<AutocompleteMatch> autocomplete(const std::string& partial) const;

  /// Get a command by name.
  std::shared_ptr<ICommand> getCommand(const std::string& name) const;

  /// List all registered commands (name + description).
  struct CommandEntry {
    std::string name;
    std::string description;
    bool isWorkflow = false;
  };
  std::vector<CommandEntry> listCommands() const;

private:
  std::vector<std::string> tokenize(const std::string& input) const;

  std::map<std::string, std::shared_ptr<ICommand>> commands_;
};

} // namespace firmius::tui2

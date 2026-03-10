#pragma once

#include "ICommand.hpp"
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace firmius::tui {

struct AutocompleteMatch {
  std::string name;
  std::string description;
  bool is_exact = false;
};

struct AutocompleteState {
  bool is_typing_command_name = true;

  // Valid when typing the command name
  std::vector<AutocompleteMatch> command_matches;

  // Valid when typing arguments for a matched command
  std::string active_command_name;
  std::optional<CommandArg> current_arg;
  std::string current_arg_value;
  bool has_current_arg_value = false;
};

class CommandManager {
public:
  static CommandManager &instance();

  void registerCommand(std::shared_ptr<ICommand> command);

  // Returns autocomplete state if the input starts with '/'
  std::optional<AutocompleteState> getAutocomplete(const std::string &input);

  // Parse and execute a command string
  // Returns true if a command was executed, false otherwise
  bool executeCommand(CommandCtx &ctx, const std::string &input);

private:
  CommandManager() = default;
  ~CommandManager() = default;

  std::map<std::string, std::shared_ptr<ICommand>> commands_;

  // Helper to tokenize arguments
  std::vector<std::string> tokenizeArgs(const std::string &arg_string);
};

} // namespace firmius::tui

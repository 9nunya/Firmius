#include "commands/CommandManager.hpp"
#include <algorithm>
#include <cctype>
#include <sstream>

namespace firmius::tui {

CommandManager &CommandManager::instance() {
  static CommandManager inst;
  return inst;
}

void CommandManager::registerCommand(std::shared_ptr<ICommand> command) {
  if (command) {
    commands_[command->name()] = command;
  }
}

// Simple helper to check if text contains letters from query in order (fuzzy
// match) Or just basic substring for simplicity. Let's do a substring match for
// now.
static bool fuzzyMatch(const std::string &text, const std::string &query) {
  if (query.empty())
    return true;
  auto it = std::search(text.begin(), text.end(), query.begin(), query.end(),
                        [](char ch1, char ch2) {
                          return std::tolower(ch1) == std::tolower(ch2);
                        });
  return it != text.end();
}

std::vector<std::string>
CommandManager::tokenizeArgs(const std::string &arg_string) {
  std::vector<std::string> args;
  std::string current;
  bool in_quotes = false;

  for (char c : arg_string) {
    if (c == '"') {
      in_quotes = !in_quotes;
    } else if (c == ' ' && !in_quotes) {
      if (!current.empty()) {
        args.push_back(current);
        current.clear();
      }
    } else {
      current.push_back(c);
    }
  }
  if (!current.empty() || arg_string.back() == ' ') {
    args.push_back(current); // Even if empty, it represents the argument
                             // actively being typed
  }
  return args;
}

std::optional<AutocompleteState>
CommandManager::getAutocomplete(const std::string &input) {
  if (input.empty() || input[0] != '/') {
    return std::nullopt;
  }

  AutocompleteState state;
  state.current_arg_value.clear();
  state.has_current_arg_value = false;
  std::string content = input.substr(1);

  size_t space_pos = content.find(' ');
  if (space_pos == std::string::npos) {
    // Still typing the command name
    state.is_typing_command_name = true;
    for (const auto &[name, cmd] : commands_) {
      if (fuzzyMatch(name, content)) {
        state.command_matches.push_back(
            {name, cmd->description(), name == content});
      }
    }
    return state;
  }

  // Space found, typing arguments
  state.is_typing_command_name = false;
  std::string cmd_name = content.substr(0, space_pos);
  state.active_command_name = cmd_name;

  auto it = commands_.find(cmd_name);
  if (it != commands_.end()) {
    std::string args_str = content.substr(space_pos + 1);
    auto tokens = tokenizeArgs(args_str);

    size_t current_arg_index = tokens.empty() ? 0 : tokens.size() - 1;
    // if the string ends in a space, we are typing the NEXT arg
    if (!args_str.empty() && args_str.back() == ' ' && !tokens.empty() &&
        !tokens.back().empty()) {
      current_arg_index++;
    }

    const auto &schema_args = it->second->args();
    if (current_arg_index < schema_args.size()) {
      state.current_arg = schema_args[current_arg_index];
      if (current_arg_index < tokens.size()) {
        state.current_arg_value = tokens[current_arg_index];
        state.has_current_arg_value = true;
      } else {
        state.current_arg_value.clear();
        state.has_current_arg_value = false;
      }
    }
  }

  return state;
}

bool CommandManager::executeCommand(CommandCtx &ctx, const std::string &input) {
  if (input.empty() || input[0] != '/') {
    return false;
  }

  std::string content = input.substr(1);
  size_t space_pos = content.find(' ');
  std::string cmd_name =
      (space_pos == std::string::npos) ? content : content.substr(0, space_pos);

  auto it = commands_.find(cmd_name);
  if (it == commands_.end()) {
    return false; // Unknown command
  }

  std::vector<std::string> tokens;
  if (space_pos != std::string::npos) {
    tokens = tokenizeArgs(content.substr(space_pos + 1));
    // Remove trailing empty token caused by spacing
    if (!tokens.empty() && tokens.back().empty()) {
      tokens.pop_back();
    }
  }

  std::vector<ParsedArg> parsed;
  const auto &schema = it->second->args();

  for (size_t i = 0; i < schema.size(); ++i) {
    if (i < tokens.size()) {
      parsed.push_back({schema[i].type, tokens[i]});
    } else if (!schema[i].optional) {
      // Missing required argument
      return false;
    } else {
      // Missing optional argument
      parsed.push_back({schema[i].type, ""});
    }
  }

  it->second->execute(ctx, parsed);
  return true;
}

std::shared_ptr<ICommand> CommandManager::getCommand(const std::string &name) const {
  auto it = commands_.find(name);
  if (it != commands_.end()) {
    return it->second;
  }
  return nullptr;
}

} // namespace firmius::tui

#include "CommandManager.hpp"

#include <algorithm>
#include <sstream>

namespace firmius::tui2 {

void CommandManager::registerCommand(std::shared_ptr<ICommand> command) {
  commands_[command->name()] = std::move(command);
}

bool CommandManager::execute(const std::string& input) {
  if (input.empty() || input[0] != '/') return false;

  std::string stripped = input.substr(1); // Remove leading '/'.

  // Find the command name (first token).
  auto tokens = tokenize(stripped);
  if (tokens.empty()) return false;

  std::string cmdName = tokens[0];
  auto it = commands_.find(cmdName);
  if (it == commands_.end()) return false;

  auto& cmd = it->second;
  auto argDefs = cmd->args();

  // Build parsed args.
  std::vector<ParsedArg> parsed;

  if (cmd->takesRawRemainder()) {
    // Everything after the command name is one raw argument.
    size_t nameEnd = stripped.find(cmdName);
    if (nameEnd != std::string::npos) {
      size_t start = nameEnd + cmdName.size();
      while (start < stripped.size() && stripped[start] == ' ') ++start;
      if (start < stripped.size()) {
        ParsedArg arg;
        arg.type = argDefs.empty() ? ArgType::String : argDefs[0].type;
        arg.rawValue = stripped.substr(start);
        parsed.push_back(std::move(arg));
      }
    }
  } else {
    // Map tokens[1..] to arg definitions.
    for (size_t i = 1; i < tokens.size() && (i - 1) < argDefs.size(); ++i) {
      ParsedArg arg;
      arg.type = argDefs[i - 1].type;
      arg.rawValue = tokens[i];
      parsed.push_back(std::move(arg));
    }
  }

  cmd->execute(parsed);
  return true;
}

std::vector<AutocompleteMatch>
CommandManager::autocomplete(const std::string& partial) const {
  std::vector<AutocompleteMatch> matches;

  for (const auto& [name, cmd] : commands_) {
    if (name.find(partial) == 0) {
      AutocompleteMatch m;
      m.name = name;
      m.description = cmd->description();
      m.isExact = (name == partial);
      matches.push_back(std::move(m));
    }
  }

  std::sort(matches.begin(), matches.end(),
            [](const auto& a, const auto& b) { return a.name < b.name; });
  return matches;
}

std::shared_ptr<ICommand>
CommandManager::getCommand(const std::string& name) const {
  auto it = commands_.find(name);
  return it != commands_.end() ? it->second : nullptr;
}

std::vector<CommandManager::CommandEntry> CommandManager::listCommands() const {
  std::vector<CommandEntry> entries;
  entries.reserve(commands_.size());
  for (const auto& [name, cmd] : commands_) {
    entries.push_back({name, cmd->description(), cmd->isWorkflow()});
  }
  return entries;
}

std::vector<std::string>
CommandManager::tokenize(const std::string& input) const {
  std::vector<std::string> tokens;
  std::istringstream stream(input);
  std::string token;
  while (stream >> token) {
    tokens.push_back(std::move(token));
  }
  return tokens;
}

} // namespace firmius::tui2

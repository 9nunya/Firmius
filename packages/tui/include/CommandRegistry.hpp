#pragma once

#include <string>
#include <vector>
#include <functional>
#include <optional>

namespace firmius::tui {

/**
 * @brief Represents a slash command in the TUI.
 */
struct Command {
    std::string name;           ///< The command name (e.g., "model")
    std::string description;    ///< Brief description of what it does
    std::string usage;          ///< Usage string
    std::function<void(const std::vector<std::string>&)> handler; ///< Execution logic
};

/**
 * @brief Registry for slash commands.
 * 
 * Handles registration, parsing, and execution of commands starting with '/'.
 */
class CommandRegistry {
public:
    CommandRegistry();

    /**
     * @brief Register a new command.
     */
    void registerCommand(Command cmd);

    /**
     * @brief Initialize default commands with dependencies.
     */
    void init(class ModalSystem& modalSystem);

    /**
     * @brief Find a command by name.
     */
    std::optional<Command> findCommand(const std::string& name) const;

    /**
     * @brief Get all registered commands.
     */
    const std::vector<Command>& getCommands() const { return commands_; }

    /**
     * @brief Parse and execute a command line.
     * @param line The full input line (including the '/')
     * @return true if it was a command and was handled, false otherwise.
     */
    bool execute(const std::string& line);

private:
    std::vector<Command> commands_;
};

} // namespace firmius::tui

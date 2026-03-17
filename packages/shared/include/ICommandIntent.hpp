#ifndef FIRMIUS_SHARED_ICOMMAND_INTENT_HPP
#define FIRMIUS_SHARED_ICOMMAND_INTENT_HPP

#include <string>
#include <vector>
#include <map>
#include <optional>

namespace firmius::shared {

/**
 * @brief Severity levels for command execution risk assessment.
 */
enum class CommandSeverity {
    LOW,        ///< Safe operations (ls, cat, grep, etc.)
    MEDIUM,     ///< Potentially risky (write operations, network)
    HIGH,       ///< Dangerous operations (rm, git reset, etc.)
    VULNERABLE  ///< System-destructive (rm -rf /, format, etc.) - NEVER ALLOW
};

/**
 * @brief Represents the intent and risk of a parsed bash command.
 */
struct CommandIntent {
    /**
     * @brief The original command string.
     */
    std::string originalCommand;
    
    /**
     * @brief Individual parsed sub-commands (split on ;, &&, ||, etc.).
     */
    std::vector<std::string> parsedCommands;
    
    /**
     * @brief The primary command (first command in chain).
     */
    std::string primaryCommand;
    
    /**
     * @brief Command arguments.
     */
    std::vector<std::string> arguments;
    
    /**
     * @brief Environment variables referenced or set.
     */
    std::map<std::string, std::string> environmentVariables;
    
    /**
     * @brief Files that will be read by this command.
     */
    std::vector<std::string> filesRead;
    
    /**
     * @brief Files that will be written/modified by this command.
     */
    std::vector<std::string> filesWritten;
    
    /**
     * @brief Directories that will be affected.
     */
    std::vector<std::string> directoriesAffected;
    
    /**
     * @brief Whether this command uses sudo or similar elevation.
     */
    bool usesElevation = false;
    
    /**
     * @brief Whether this command has destructive potential.
     */
    bool isDestructive = false;
    
    /**
     * @brief Whether this command uses pipes or subshells.
     */
    bool hasPipesOrSubshells = false;
    
    /**
     * @brief The assessed severity level.
     */
    CommandSeverity severity = CommandSeverity::LOW;
    
    /**
     * @brief Human-readable summary of what this command does.
     */
    std::string summary;
    
    /**
     * @brief Specific reason for the severity assessment.
     */
    std::string severityReason;
};

/**
 * @brief Interface for analyzing bash commands to determine intent and severity.
 */
class ICommandIntentAnalyzer {
public:
    virtual ~ICommandIntentAnalyzer() = default;
    
    /**
     * @brief Analyzes a command string to determine its intent and severity.
     * @param command The bash command to analyze.
     * @param cwd The current working directory (for path resolution).
     * @return The analyzed command intent.
     */
    virtual CommandIntent analyze(const std::string& command, const std::string& cwd = "") const = 0;
    
    /**
     * @brief Assesses severity of an already-parsed CommandIntent.
     * @param intent The parsed command intent to assess.
     * @return The severity level (also sets intent.severity and intent.severityReason).
     */
    virtual CommandSeverity assessSeverity(CommandIntent& intent) const = 0;
};

} // namespace firmius::shared

#endif // FIRMIUS_SHARED_ICOMMAND_INTENT_HPP
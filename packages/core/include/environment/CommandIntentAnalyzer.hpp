#ifndef FIRMIUS_CORE_COMMAND_INTENT_ANALYZER_HPP
#define FIRMIUS_CORE_COMMAND_INTENT_ANALYZER_HPP

#include "ICommandIntent.hpp"
#include <set>
#include <regex>

namespace firmius::core {

using firmius::shared::CommandIntent;
using firmius::shared::CommandSeverity;
using firmius::shared::ICommandIntentAnalyzer;

/**
 * @brief Analyzes bash commands to extract intent and assess security severity.
 * 
 * Implements ICommandIntentAnalyzer with modular severity assessment:
 * 1. Parse command into CommandIntent (subcommands, files, dirs)
 * 2. Assess severity using multiple specialized checks
 * 3. Aggregate results into final severity rating
 */
class CommandIntentAnalyzer : public ICommandIntentAnalyzer {
public:
    CommandIntentAnalyzer();
    
    // ICommandIntentAnalyzer implementation
    CommandIntent analyze(const std::string& command, const std::string& cwd = "") const override;
    CommandSeverity assessSeverity(CommandIntent& intent) const override;

private:
    // Command parsing helpers
    std::vector<std::string> splitCommandChain(const std::string& command) const;
    void parseSingleCommand(const std::string& cmd, CommandIntent& intent) const;
    std::vector<std::string> tokenize(const std::string& cmd) const;
    
    // Path extraction helpers
    void extractPathsFromCommand(const std::vector<std::string>& tokens, 
                                  const std::string& cwd,
                                  CommandIntent& intent) const;
    void resolveAndCategorizePath(const std::string& path,
                                   const std::string& cwd,
                                   bool isWrite,
                                   CommandIntent& intent) const;
    
    // Environment variable extraction
    void extractEnvironmentVariables(const std::string& command, CommandIntent& intent) const;
    
    // Severity assessment helpers (modular approach)
    bool checkVulnerablePatterns(CommandIntent& intent) const;
    bool checkDestructivePatterns(CommandIntent& intent) const;
    bool checkSubshellVulnerabilities(const std::string& command, CommandIntent& intent) const;
    bool checkElevatedPrivileges(const CommandIntent& intent) const;
    bool checkSystemCriticalPaths(const CommandIntent& intent) const;
    bool checkGitDestructiveCommands(const CommandIntent& intent) const;
    bool checkRecursiveDeletion(const CommandIntent& intent) const;
    bool checkBroadWriteOperations(const CommandIntent& intent) const;
    
    // Severity determination
    CommandSeverity determineFinalSeverity(const CommandIntent& intent,
                                            bool isVulnerable,
                                            bool isDestructive) const;

    // Summary generation
    void generateSummary(CommandIntent& intent) const;
    
    // Pattern sets (initialized in constructor)
    std::set<std::string> vulnerableCommands_;
    std::set<std::string> destructiveCommands_;
    std::set<std::string> elevatedPrefixes_;
    std::set<std::string> systemCriticalPaths_;
    std::set<std::string> gitDestructiveCommands_;
    std::set<std::string> writeCommands_;
    std::set<std::string> readCommands_;
    
    // Regex patterns
    std::regex subshellRegex_;
    std::regex pipeRegex_;
    std::regex envVarRegex_;
    std::regex pathRegex_;
};

} // namespace firmius::core

#endif // FIRMIUS_CORE_COMMAND_INTENT_ANALYZER_HPP
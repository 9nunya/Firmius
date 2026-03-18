#include "environment/CommandIntentAnalyzer.hpp"
#include "utils/StringUtil.hpp"
#include <algorithm>
#include <cctype>
#include <sstream>
#include <filesystem>

namespace firmius::core {

using namespace firmius::shared;

namespace {

std::string stripMatchingQuotes(const std::string& token) {
    if (token.size() >= 2 &&
        ((token.front() == '"' && token.back() == '"') ||
         (token.front() == '\'' && token.back() == '\''))) {
        return token.substr(1, token.size() - 2);
    }
    return token;
}

std::string resolveWorkingDirectory(const std::string& path, const std::string& cwd) {
    std::string resolved = path;

    if (!resolved.empty() && resolved[0] == '~') {
        const char* home = std::getenv("HOME");
        if (home) {
            resolved = std::string(home) + resolved.substr(1);
        }
    }

    if (!resolved.empty() && resolved[0] != '/' && !cwd.empty()) {
        resolved = cwd + "/" + resolved;
    }

    try {
        resolved = std::filesystem::weakly_canonical(resolved).string();
    } catch (...) {
    }

    return resolved;
}

}

CommandIntentAnalyzer::CommandIntentAnalyzer()
    : subshellRegex_(R"(\$\(|\`|\$\{)")
    , pipeRegex_(R"(\|\s*)")
    , envVarRegex_(R"((\w+)=([^\s]+))")
    , pathRegex_(R"((?:/[^/\s]+)+|\.?\.?/[^\s]*)")
{
    // Vulnerable commands that are NEVER allowed
    vulnerableCommands_ = {
        "rm",
        "mkfs",
        "dd",
        "format",
        "fdisk",
        "shutdown",
        "reboot",
        "init",
        "systemctl",
    };

    // Destructive commands that need careful checking
    destructiveCommands_ = {
        "rm", "rmdir", "unlink",
        "git",
        "truncate",
        "shred",
    };

    // Commands that typically require elevation
    elevatedPrefixes_ = {
        "sudo", "su", "doas", "pkexec"
    };

    // System-critical paths
    systemCriticalPaths_ = {
        "/", "/bin", "/sbin", "/usr", "/etc",
        "/lib", "/lib64", "/boot", "/dev",
        "~/.ssh", "~/.gnupg", "~/.config"
    };

    // Git commands with destructive potential
    gitDestructiveCommands_ = {
        "reset", "checkout", "clean", "rebase"
    };

    // Commands that write to files
    writeCommands_ = {
        "cp", "mv", "install", "dd", "tee",
        "echo", "printf", "sed", "awk"
    };

    // Commands that read from files
    readCommands_ = {
        "cat", "less", "more", "head", "tail", "grep", "awk", "sed",
        "sort", "uniq", "wc", "cut", "tr", "xargs", "find", "ls"
    };
}

CommandIntent CommandIntentAnalyzer::analyze(const std::string& command, const std::string& cwd) const {
    CommandIntent intent;
    intent.originalCommand = command;

    size_t pipePos = command.find('|');
    while (pipePos != std::string::npos) {
        if (pipePos + 1 >= command.length() || command[pipePos + 1] != '|') {
            intent.hasPipesOrSubshells = true;
            break;
        }
        pipePos = command.find('|', pipePos + 2);
    }
    if (!intent.hasPipesOrSubshells) {
        intent.hasPipesOrSubshells = std::regex_search(command, subshellRegex_);
    }

    intent.parsedCommands = splitCommandChain(command);

    if (!intent.parsedCommands.empty()) {
        parseSingleCommand(intent.parsedCommands[0], intent);
    }

    extractEnvironmentVariables(command, intent);

    // Extract paths from all sub-commands, carrying forward cwd changes.
    std::string effectiveCwd = cwd;
    for (const auto& cmd : intent.parsedCommands) {
        auto tokens = tokenize(cmd);
        extractPathsFromCommand(tokens, effectiveCwd, intent);

        if (tokens.empty()) {
            continue;
        }

        size_t idx = 0;
        if (elevatedPrefixes_.count(tokens[0]) > 0 && tokens.size() > 1) {
            idx = 1;
        }

        if (idx < tokens.size() && tokens[idx] == "cd" && idx + 1 < tokens.size()) {
            effectiveCwd = resolveWorkingDirectory(stripMatchingQuotes(tokens[idx + 1]),
                                                   effectiveCwd);
        }
    }

    // Assess severity
    assessSeverity(intent);

    // Generate summary
    generateSummary(intent);

    return intent;
}

CommandSeverity CommandIntentAnalyzer::assessSeverity(CommandIntent& intent) const {
    bool isVulnerable = checkVulnerablePatterns(intent);
    bool isDestructivePattern = checkDestructivePatterns(intent);

    if (isVulnerable) {
        intent.severity = CommandSeverity::VULNERABLE;
        intent.isDestructive = true;
    } else if (isDestructivePattern || intent.isDestructive) {
        intent.severity = CommandSeverity::HIGH;
        intent.isDestructive = true;
    } else if (intent.usesElevation) {
        intent.severity = CommandSeverity::MEDIUM;
    } else if (!intent.filesWritten.empty() || intent.hasPipesOrSubshells) {
        intent.severity = CommandSeverity::MEDIUM;
    } else {
        intent.severity = CommandSeverity::LOW;
    }

    return intent.severity;
}

std::vector<std::string> CommandIntentAnalyzer::splitCommandChain(const std::string& command) const {
    std::vector<std::string> commands;
    std::string current;
    int parens = 0;
    int brackets = 0;
    bool inQuote = false;
    char quoteChar = 0;

    for (size_t i = 0; i < command.length(); ++i) {
        char c = command[i];

        if ((c == '"' || c == '\'') && !inQuote) {
            inQuote = true;
            quoteChar = c;
        } else if (c == quoteChar && inQuote) {
            inQuote = false;
        }

        if (!inQuote) {
            if (c == '(' || c == '{') parens++;
            else if (c == ')' || c == '}') parens--;
            else if (c == '[') brackets++;
            else if (c == ']') brackets--;
        }

        if (parens == 0 && brackets == 0 && !inQuote) {
            if (c == ';') {
                if (!current.empty()) {
                    std::string trimmed = StringUtil::trim(current);
                    if (!trimmed.empty()) {
                        commands.push_back(trimmed);
                    }
                    current.clear();
                }
                continue;
            } else if (c == '&' && i + 1 < command.length() && command[i + 1] == '&') {
                if (!current.empty()) {
                    std::string trimmed = StringUtil::trim(current);
                    if (!trimmed.empty()) {
                        commands.push_back(trimmed);
                    }
                    current.clear();
                }
                i++;
                continue;
            } else if (c == '|' && i + 1 < command.length() && command[i + 1] == '|') {
                if (!current.empty()) {
                    std::string trimmed = StringUtil::trim(current);
                    if (!trimmed.empty()) {
                        commands.push_back(trimmed);
                    }
                    current.clear();
                }
                i++;
                continue;
            } else if (c == '|') {
                if (!current.empty()) {
                    std::string trimmed = StringUtil::trim(current);
                    if (!trimmed.empty()) {
                        commands.push_back(trimmed);
                    }
                    current.clear();
                }
                continue;
            }
        }

        current += c;
    }

    if (!current.empty()) {
        std::string trimmed = StringUtil::trim(current);
        if (!trimmed.empty()) {
            commands.push_back(trimmed);
        }
    }

    return commands;
}

void CommandIntentAnalyzer::parseSingleCommand(const std::string& cmd, CommandIntent& intent) const {
    auto tokens = tokenize(cmd);
    if (tokens.empty()) return;

    size_t idx = 0;

    // Check for elevation
    if (elevatedPrefixes_.count(tokens[0]) > 0) {
        intent.usesElevation = true;
        idx = 1;
    }

    if (idx < tokens.size()) {
        intent.primaryCommand = tokens[idx];

        // Collect arguments (preserve quotes)
        for (size_t i = idx + 1; i < tokens.size(); ++i) {
            intent.arguments.push_back(tokens[i]);
        }
    }

    // Check for pipes/subshells using the ORIGINAL command string
    // Single pipe detection - look for | but not ||
    size_t pipePos = cmd.find('|');
    while (pipePos != std::string::npos) {
        // Check if it's not ||
        if (pipePos + 1 >= cmd.length() || cmd[pipePos + 1] != '|') {
            intent.hasPipesOrSubshells = true;
            break;
        }
        // Skip the || and continue searching
        pipePos = cmd.find('|', pipePos + 2);
    }

    // Check for subshells
    if (!intent.hasPipesOrSubshells) {
        if (std::regex_search(cmd, subshellRegex_)) {
            intent.hasPipesOrSubshells = true;
        }
    }
}

std::vector<std::string> CommandIntentAnalyzer::tokenize(const std::string& cmd) const {
    std::vector<std::string> tokens;
    std::string current;
    bool inQuote = false;
    char quoteChar = 0;

    for (size_t i = 0; i < cmd.length(); ++i) {
        char c = cmd[i];

        if ((c == '"' || c == '\'') && !inQuote) {
            // Starting a quote - include the quote char
            inQuote = true;
            quoteChar = c;
            if (!current.empty()) {
                tokens.push_back(current);
                current.clear();
            }
            current += c;
        } else if (c == quoteChar && inQuote) {
            // Ending a quote - include the quote char
            inQuote = false;
            current += c;
            tokens.push_back(current);
            current.clear();
        } else if (std::isspace(c) && !inQuote) {
            if (!current.empty()) {
                tokens.push_back(current);
                current.clear();
            }
        } else {
            current += c;
        }
    }

    if (!current.empty()) {
        tokens.push_back(current);
    }

    return tokens;
}

void CommandIntentAnalyzer::extractPathsFromCommand(const std::vector<std::string>& tokens,
                                                     const std::string& cwd,
                                                     CommandIntent& intent) const {
    if (tokens.empty()) return;

    std::string cmd = tokens[0];
    if (elevatedPrefixes_.count(cmd) > 0 && tokens.size() > 1) {
        cmd = tokens[1];
    }

    bool isWriteOp = writeCommands_.count(cmd) > 0;

    if (cmd == "rm" || cmd == "rmdir") {
        isWriteOp = true;
    }

    if (cmd == "cd") {
        return;
    }

    std::vector<std::string> sourceFiles;
    bool isCopyMove = (cmd == "cp" || cmd == "mv");
    bool takesFileArgs = readCommands_.count(cmd) > 0 || writeCommands_.count(cmd) > 0 ||
                         cmd == "tee" || cmd == "cat";
    bool skippedPrimaryOperand = false;

    for (size_t i = 1; i < tokens.size(); ++i) {
        const std::string& token = tokens[i];

        if (token.empty()) continue;

        if (token[0] == '-' && token.length() > 1 && !std::isdigit(token[1])) {
            continue;
        }

        std::string unquoted = stripMatchingQuotes(token);

        bool looksLikePath = !unquoted.empty() &&
            (unquoted[0] == '/' || unquoted[0] == '.' || unquoted[0] == '~' ||
             unquoted.find('/') != std::string::npos);

        if ((cmd == "grep" || cmd == "sed" || cmd == "awk") &&
            !skippedPrimaryOperand && !looksLikePath) {
            skippedPrimaryOperand = true;
            continue;
        }

        if (looksLikePath || isCopyMove || takesFileArgs) {
            if (isCopyMove) {
                bool isLastToken = (i == tokens.size() - 1);
                if (!isLastToken) {
                    sourceFiles.push_back(unquoted);
                    resolveAndCategorizePath(unquoted, cwd, false, intent);
                } else {
                    if (!unquoted.empty() && unquoted.back() == '/' && !sourceFiles.empty()) {
                        for (const auto& src : sourceFiles) {
                            std::filesystem::path srcPath(src);
                            std::string destPath = unquoted + srcPath.filename().string();
                            resolveAndCategorizePath(destPath, cwd, true, intent);
                        }
                    } else {
                        resolveAndCategorizePath(unquoted, cwd, true, intent);
                    }
                }
            } else {
                resolveAndCategorizePath(unquoted, cwd, isWriteOp, intent);
            }
        }
    }
}

void CommandIntentAnalyzer::resolveAndCategorizePath(const std::string& path,
                                                      const std::string& cwd,
                                                      bool isWrite,
                                                      CommandIntent& intent) const {
    std::string resolved = resolveWorkingDirectory(path, cwd);

    if (isWrite) {
        intent.filesWritten.push_back(resolved);
    } else {
        intent.filesRead.push_back(resolved);
    }

    // Extract directory
    try {
        std::filesystem::path p(resolved);
        auto parent = p.parent_path();
        if (!parent.empty()) {
            intent.directoriesAffected.push_back(parent.string());
        }
    } catch (...) {}
}

void CommandIntentAnalyzer::extractEnvironmentVariables(const std::string& command, CommandIntent& intent) const {
    std::smatch match;
    std::string::const_iterator searchStart(command.cbegin());

    while (std::regex_search(searchStart, command.cend(), match, envVarRegex_)) {
        if (match[1].matched && match[2].matched) {
            intent.environmentVariables[match[1]] = match[2];
        }
        searchStart = match.suffix().first;
    }

    std::regex refRegex(R"(\$(\w+))");
    searchStart = command.cbegin();
    while (std::regex_search(searchStart, command.cend(), match, refRegex)) {
        if (match[1].matched) {
            std::string varName = match[1];
            if (intent.environmentVariables.find(varName) == intent.environmentVariables.end()) {
                intent.environmentVariables[varName] = "";
            }
        }
        searchStart = match.suffix().first;
    }
}

bool CommandIntentAnalyzer::checkVulnerablePatterns(CommandIntent& intent) const {
    if (checkSubshellVulnerabilities(intent.originalCommand, intent)) {
        return true;
    }

    for (const auto& cmd : intent.parsedCommands) {
        auto tokens = tokenize(cmd);
        if (tokens.empty()) continue;

        size_t idx = 0;
        if (elevatedPrefixes_.count(tokens[0]) > 0) {
            if (tokens.size() > 1) idx = 1;
            else continue;
        }

        std::string primary = tokens[idx];

        // Check rm -rf / or rm -rf ~/
        if (primary == "rm") {
            bool hasRecursive = false;
            bool hasForce = false;
            bool targetsRoot = false;

            for (size_t i = idx + 1; i < tokens.size(); ++i) {
                const std::string& arg = tokens[i];
                // Check for -r, -f, -rf, -fr, -rF, etc.
                if (arg.size() >= 2 && arg[0] == '-') {
                    std::string flags = arg.substr(1);
                    if (flags.find('r') != std::string::npos) hasRecursive = true;
                    if (flags.find('f') != std::string::npos) hasForce = true;
                }

                // Check for targets
                if (arg == "/" || arg == "~" || arg == "~/" ||
                    arg == "$HOME" || arg == "$HOME/" || arg == "/home") {
                    targetsRoot = true;
                }
            }

            if (hasRecursive && hasForce && targetsRoot) {
                intent.severityReason = "Attempted recursive deletion of system directory";
                return true;
            }
        }

        // Check git reset --hard or git checkout with force
        if (primary == "git") {
            for (size_t i = idx + 1; i < tokens.size(); ++i) {
                if (tokens[i] == "reset" || tokens[i] == "checkout") {
                    for (size_t j = i + 1; j < tokens.size(); ++j) {
                        if (tokens[j] == "--hard" || tokens[j] == "-f" || tokens[j] == "--force") {
                            intent.severityReason = "Destructive git operation that may lose work";
                            return true;
                        }
                    }
                }
            }
        }

        // Check for dd with of=/dev/
        if (primary == "dd") {
            for (const auto& arg : tokens) {
                if (arg.find("of=/dev/") == 0 || arg.find("of=/disk") == 0) {
                    intent.severityReason = "Direct disk write operation";
                    return true;
                }
            }
        }
    }

    return false;
}

bool CommandIntentAnalyzer::checkSubshellVulnerabilities(const std::string& command, CommandIntent& intent) const {
    std::regex subshellExtractRegex(R"(\$\(([^)]+)\)|`([^`]+)`)");
    std::smatch match;
    std::string::const_iterator searchStart(command.cbegin());

    while (std::regex_search(searchStart, command.cend(), match, subshellExtractRegex)) {
        std::string subshellContent;
        if (match[1].matched) {
            subshellContent = match[1];
        } else if (match[2].matched) {
            subshellContent = match[2];
        }

        if (!subshellContent.empty()) {
            CommandIntent subshellIntent = analyze(subshellContent, "");
            if (subshellIntent.severity == CommandSeverity::VULNERABLE) {
                intent.severityReason = "Vulnerable command in subshell: " + subshellIntent.severityReason;
                return true;
            }
            if (subshellIntent.usesElevation && subshellIntent.isDestructive) {
                intent.severityReason = "Elevated destructive command in subshell";
                return true;
            }
        }
        searchStart = match.suffix().first;
    }

    return false;
}

bool CommandIntentAnalyzer::checkDestructivePatterns(CommandIntent& intent) const {
    for (const auto& cmd : intent.parsedCommands) {
        auto tokens = tokenize(cmd);
        if (tokens.empty()) continue;

        size_t idx = 0;
        if (elevatedPrefixes_.count(tokens[0]) > 0) {
            if (tokens.size() > 1) idx = 1;
            else continue;
        }

        std::string primary = tokens[idx];

        // Check rm without -rf / but still destructive
        if (primary == "rm" || primary == "rmdir") {
            // Any rm command is destructive
            // Check if there are file arguments (non-flag tokens after the command)
            for (size_t i = idx + 1; i < tokens.size(); ++i) {
                const std::string& arg = tokens[i];
                if (!arg.empty() && arg[0] != '-') {
                    return true;
                }
            }
        }

        // Check git operations
        if (primary == "git") {
            for (size_t i = idx + 1; i < tokens.size(); ++i) {
                if (gitDestructiveCommands_.count(tokens[i]) > 0) {
                    // Only mark as destructive if not already marked as vulnerable
                    if (intent.severity != CommandSeverity::VULNERABLE) {
                        return true;
                    }
                }
            }
        }
    }

    return false;
}

void CommandIntentAnalyzer::generateSummary(CommandIntent& intent) const {
    std::ostringstream summary;

    if (intent.primaryCommand.empty()) {
        intent.summary = "Empty command";
        return;
    }

    summary << "Command: " << intent.primaryCommand;

    if (!intent.arguments.empty()) {
        summary << " with " << intent.arguments.size() << " argument(s)";
    }

    if (intent.usesElevation) {
        summary << " (elevated privileges)";
    }

    if (intent.hasPipesOrSubshells) {
        summary << " (complex pipeline)";
    }

    if (!intent.filesRead.empty()) {
        summary << ", reads " << intent.filesRead.size() << " file(s)";
    }

    if (!intent.filesWritten.empty()) {
        summary << ", writes " << intent.filesWritten.size() << " file(s)";
    }

    switch (intent.severity) {
        case CommandSeverity::VULNERABLE:
            summary << " [VULNERABLE]";
            break;
        case CommandSeverity::HIGH:
            summary << " [HIGH RISK]";
            break;
        case CommandSeverity::MEDIUM:
            summary << " [MEDIUM RISK]";
            break;
        default:
            break;
    }

    intent.summary = summary.str();
}

} // namespace firmius::core

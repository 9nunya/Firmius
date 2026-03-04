#include "CommandRegistry.hpp"
#include "utils/StringUtil.hpp"
#include <sstream>
#include <algorithm>

namespace firmius::tui {

CommandRegistry::CommandRegistry() {
    registerCommand({"model", "Switch the current model", "/model <model_id>", [](const std::vector<std::string>& args) {
        (void)args;
    }});

    registerCommand({"models", "List available models", "/models", [](const std::vector<std::string>& args) {
        (void)args;
    }});

    registerCommand({"thread", "Switch to a specific thread", "/thread <thread_id>", [](const std::vector<std::string>& args) {
        (void)args;
    }});

    registerCommand({"threads", "List all threads", "/threads", [](const std::vector<std::string>& args) {
        (void)args;
    }});

    registerCommand({"config", "Show or update configuration", "/config [key] [value]", [](const std::vector<std::string>& args) {
        (void)args;
    }});

    registerCommand({"undo", "Undo the last message", "/undo", [](const std::vector<std::string>& args) {
        (void)args;
    }});

    registerCommand({"new", "Start a new thread", "/new", [](const std::vector<std::string>& args) {
        (void)args;
    }});

    registerCommand({"clear", "Clear the current screen", "/clear", [](const std::vector<std::string>& args) {
        (void)args;
    }});

    registerCommand({"compact", "Manually trigger context compaction", "/compact", [](const std::vector<std::string>& args) {
        (void)args;
    }});

    registerCommand({"focus", "Focus on a specific agent", "/focus <agent_id>", [](const std::vector<std::string>& args) {
        (void)args;
    }});
}

void CommandRegistry::registerCommand(Command cmd) {
    commands_.push_back(std::move(cmd));
}

std::optional<Command> CommandRegistry::findCommand(const std::string& name) const {
    for (const auto& cmd : commands_) {
        if (cmd.name == name) {
            return cmd;
        }
    }

    float bestThreshold = 0.0f;
    const Command* bestMatch = nullptr;

    for (const auto& cmd : commands_) {
        auto matches = firmius::shared::StringUtil::findFuzzy(cmd.name, name, 0.7f);
        if (!matches.empty()) {
            size_t dist = firmius::shared::StringUtil::levenshteinDistance(cmd.name, name);
            float score = 1.0f - (float)dist / (float)std::max(cmd.name.length(), name.length());
            if (score > bestThreshold) {
                bestThreshold = score;
                bestMatch = &cmd;
            }
        }
    }

    if (bestThreshold >= 0.7f && bestMatch) {
        return *bestMatch;
    }

    return std::nullopt;
}

bool CommandRegistry::execute(const std::string& line) {
    if (line.empty() || line[0] != '/') {
        return false;
    }

    std::string content = line.substr(1);
    std::vector<std::string> parts = firmius::shared::StringUtil::split(content, ' ');
    if (parts.empty()) {
        return false;
    }

    std::string cmdName = parts[0];
    parts.erase(parts.begin());

    auto cmd = findCommand(cmdName);
    if (cmd) {
        cmd->handler(parts);
        return true;
    }

    return false;
}

}

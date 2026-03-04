#include "CommandRegistry.hpp"
#include "utils/StringUtil.hpp"
#include <sstream>
#include <algorithm>

#include "ModalSystem.hpp"
#include "harness/Harness.hpp"
#include "Engine.hpp"

namespace firmius::tui {

CommandRegistry::CommandRegistry() {}

void CommandRegistry::init(ModalSystem& modalSystem) {
    registerCommand({"model", "Switch the current model", "/model <provider>://<model>", [](const std::vector<std::string>& args) {
        if (args.size() == 1) {
            std::string full = args[0];
            size_t pos = full.find("://");
            if (pos != std::string::npos) {
                std::string prov = full.substr(0, pos);
                std::string mod = full.substr(pos + 3);
                core::Harness::instance().switchModel(prov, mod);
            }
        }
    }});

    registerCommand({"models", "List available models", "/models", [&modalSystem](const std::vector<std::string>&) {
        modalSystem.show(ModalType::ModelSelector);
    }});

    registerCommand({"thread", "Switch to a specific thread", "/thread <thread_id>", [](const std::vector<std::string>& args) {
        if (!args.empty()) {
            core::Harness::instance().switchThread(args[0]);
        }
    }});

    registerCommand({"threads", "List all threads", "/threads", [&modalSystem](const std::vector<std::string>&) {
        modalSystem.show(ModalType::ThreadSwitcher);
    }});

    registerCommand({"config", "Show configuration editor", "/config", [&modalSystem](const std::vector<std::string>&) {
        // modalSystem.show(ModalType::ConfigEditor); // If implemented
    }});

    registerCommand({"undo", "Undo last N turns", "/undo [n]", [](const std::vector<std::string>& args) {
        int count = 1;
        if (!args.empty()) {
            try { count = std::stoi(args[0]); } catch (...) {}
        }
        core::Harness::instance().undoTurns(count);
    }});

    registerCommand({"new", "Start a new thread", "/new [cwd]", [](const std::vector<std::string>& args) {
        std::string cwd = args.empty() ? "." : args[0];
        core::Harness::instance().newThread(shared::HostType::Local, cwd);
    }});

    registerCommand({"clear", "Clear chat history", "/clear", [](const std::vector<std::string>&) {
        // TUI level clear handled via AppState if needed
    }});

    registerCommand({"compact", "Force context compaction", "/compact", [](const std::vector<std::string>&) {
    }});

    registerCommand({"focus", "Focus on a specific agent", "/focus <agent_id>", [](const std::vector<std::string>& args) {
        if (!args.empty()) {
            // Logic to find agent by friendly name and focus
        }
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

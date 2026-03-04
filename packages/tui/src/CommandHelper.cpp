#include "CommandHelper.hpp"
#include <ftxui/dom/elements.hpp>
#include "utils/StringUtil.hpp"
#include <algorithm>

namespace firmius::tui {

CommandHelper::CommandHelper(const CommandRegistry& registry, const AppState& state)
    : registry_(registry), state_(state) {}

ftxui::Element CommandHelper::render(const std::string& currentInput) {
    if (currentInput.empty() || currentInput[0] != '/') {
        return ftxui::emptyElement();
    }

    auto suggestions = getSuggestions(currentInput);
    if (suggestions.empty()) {
        return ftxui::emptyElement();
    }

    ftxui::Elements elements;
    for (const auto& s : suggestions) {
        elements.push_back(ftxui::text(s) | ftxui::border | ftxui::color(ftxui::Color::Cyan));
    }

    return ftxui::hbox(std::move(elements));
}

std::vector<std::string> CommandHelper::getSuggestions(const std::string& input) {
    std::vector<std::string> result;
    if (input.empty() || input[0] != '/') return result;

    std::string content = input.substr(1);
    auto parts = firmius::shared::StringUtil::split(content, ' ');

    if (parts.empty()) {
        for (const auto& cmd : registry_.getCommands()) {
            result.push_back("/" + cmd.name);
        }
        return result;
    }

    bool endsWithSpace = firmius::shared::StringUtil::endsWith(input, " ");

    if (parts.size() == 1 && !endsWithSpace) {
        std::string cmdPart = parts[0];
        for (const auto& cmd : registry_.getCommands()) {
            if (firmius::shared::StringUtil::startsWith(cmd.name, cmdPart)) {
                result.push_back("/" + cmd.name);
            } else {
                auto fuzzy = firmius::shared::StringUtil::findFuzzy(cmd.name, cmdPart, 0.7f);
                if (!fuzzy.empty()) {
                    result.push_back("/" + cmd.name);
                }
            }
        }
    } else {
        std::string cmdName = parts[0];
        std::string argPart = endsWithSpace ? "" : parts.back();

        if (cmdName == "model" || cmdName == "models") {
            std::vector<std::string> models = {"gpt-4o", "gpt-4-turbo", "claude-3-5-sonnet", "claude-3-opus", "gemini-1.5-pro", "llama-3-70b"};
            for (const auto& m : models) {
                if (argPart.empty() || firmius::shared::StringUtil::startsWith(m, argPart)) {
                    result.push_back(m);
                }
            }
        } else if (cmdName == "focus") {
            for (const auto& sub : state_.getSubagents()) {
                if (argPart.empty() || firmius::shared::StringUtil::startsWith(sub.agentId, argPart)) {
                    result.push_back(sub.agentId);
                }
            }
        } else if (cmdName == "thread" || cmdName == "threads") {
            auto footer = state_.getFooterInfo();
            if (!footer.threadId.empty()) {
                if (argPart.empty() || firmius::shared::StringUtil::startsWith(footer.threadId, argPart)) {
                    result.push_back(footer.threadId);
                }
            }
        }
    }

    if (result.size() > 5) {
        result.resize(5);
    }

    return result;
}

}

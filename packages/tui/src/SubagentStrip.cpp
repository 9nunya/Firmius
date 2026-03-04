#include "SubagentStrip.hpp"
#include "AppState.hpp"

#include <ftxui/component/component.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/color.hpp>
#include <cmath>
#include <chrono>

namespace firmius::tui {

using namespace ftxui;

/**
 * @brief Represents a single agent row in the SubagentStrip.
 * Handles rendering with glint animation and click events for focusing.
 */
class AgentRow : public ComponentBase {
public:
    AgentRow(std::shared_ptr<AppState> state, std::string agentId, std::string friendlyName, std::string title)
        : state_(std::move(state)), agentId_(std::move(agentId)), friendlyName_(std::move(friendlyName)), title_(std::move(title)) {}

    Element Render() override {
        bool isFocused = (state_->getFocusedAgentId() == agentId_);
        bool isActive = false;
        std::string statusStr = "IDLE";

        auto streaming = state_->getStreamingMessage();
        if (streaming && streaming->message.id == "stream-" + agentId_) {
            isActive = true;
            statusStr = "THINKING";
            if (!streaming->message.content.empty()) {
                if (std::holds_alternative<firmius::shared::ThinkingContent>(streaming->message.content.back())) {
                    statusStr = "THINKING";
                } else if (std::holds_alternative<firmius::shared::TextContent>(streaming->message.content.back())) {
                    statusStr = "STREAMING";
                }
            }
        }

        auto toolCalls = state_->getActiveToolCalls();
        for (const auto& tc : toolCalls) {
            if (tc.agentId == agentId_) {
                isActive = true;
                statusStr = "TOOL_CALL";
                break;
            }
        }

        std::string focusIndicator = isFocused ? "[>]" : "[ ]";
        ftxui::Color focusColor = isFocused ? ftxui::Color(ftxui::Color::Cyan) : ftxui::Color(ftxui::Color::Default);
        std::string rowText = focusIndicator + " " + friendlyName_ + " \"" + title_ + "\" " + statusStr;

        Element el;
        if (!isActive) {
            el = text(rowText) | ftxui::color(focusColor);
        } else {
            // Glint Animation
            auto now = std::chrono::steady_clock::now();
            auto ms_count = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
            double phase = fmod(static_cast<double>(ms_count), 2000.0) / 2000.0;

            Elements chars;
            int width = static_cast<int>(rowText.length());
            for (int i = 0; i < width; ++i) {
                std::string c(1, rowText[i]);
                chars.push_back(text(c) | bgcolor(getGlintColor(i, width, phase)));
            }
            el = hbox(std::move(chars)) | ftxui::color(focusColor);
        }

        return el | reflect(box_);
    }

    bool OnEvent(Event event) override {
        if (event.is_mouse() && box_.Contain(event.mouse().x, event.mouse().y)) {
            if (event.mouse().button == Mouse::Left && event.mouse().motion == Mouse::Pressed) {
                state_->setFocusedAgentId(agentId_);
                return true;
            }
        }
        return false;
    }

private:
    std::shared_ptr<AppState> state_;
    std::string agentId_;
    std::string friendlyName_;
    std::string title_;
    Box box_;

    ftxui::Color getGlintColor(int x, int width, double phase) {
        double center = phase * width * 1.5 - (width * 0.25);
        double distance = std::abs(static_cast<double>(x) - center);
        double sigma = width * 0.15;
        double intensity = std::exp(-(distance * distance) / (2 * sigma * sigma));

        // Subtle glint: deep blue/purple background pulse
        uint8_t r = static_cast<uint8_t>(20 * intensity);
        uint8_t g = static_cast<uint8_t>(40 * intensity);
        uint8_t b = static_cast<uint8_t>(80 * intensity);
        return ftxui::Color::RGB(r, g, b);
    }
};

SubagentStrip::SubagentStrip(std::shared_ptr<AppState> state)
    : state_(std::move(state)) {}

Element SubagentStrip::Render() {
    auto subagents = state_->getSubagents();
    
    // Rebuild components if the list of subagents changed
    if (children_.size() != subagents.size()) {
        DetachAllChildren();
        for (const auto& agent : subagents) {
            Add(std::make_shared<AgentRow>(state_, agent.agentId, agent.friendlyName, agent.title));
        }
    }

    Elements rows;
    for (auto& child : children_) {
        rows.push_back(child->Render());
    }

    if (rows.empty()) {
        return text("");
    }

    return vbox(std::move(rows));
}

bool SubagentStrip::OnEvent(Event event) {
    for (auto& child : children_) {
        if (child->OnEvent(event)) {
            return true;
        }
    }
    return false;
}

} // namespace firmius::tui

#pragma once

#include <ftxui/component/component.hpp>
#include <ftxui/component/component_base.hpp>
#include <ftxui/screen/color.hpp>
#include <memory>
#include <string>
#include <vector>
#include <chrono>

namespace firmius::tui {

class AppState;

/**
 * @brief Component that displays a list of active subagents with animation.
 *
 * Requirements:
 * - Render list of all active agents between chat and input.
 * - Row: `[>] friendlyName "title" STATUS`.
 * - `[>]` focused (Cyan), `[ ]` unfocused.
 * - Glint Animation for active agents (Streaming/Thinking/ToolCall).
 * - Click-to-focus handler.
 */
class SubagentStrip : public ftxui::ComponentBase {
public:
    explicit SubagentStrip(std::shared_ptr<AppState> state);
    ~SubagentStrip() override = default;

    ftxui::Element Render() override;
    bool OnEvent(ftxui::Event event) override;

private:
    std::shared_ptr<AppState> state_;
};

} // namespace firmius::tui

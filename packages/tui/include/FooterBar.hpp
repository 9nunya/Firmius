#pragma once

#include <ftxui/component/component.hpp>
#include <memory>
#include <string>
#include "Enums.hpp"

namespace firmius::tui {

class AppState;

/**
 * @brief Footer bar component displaying live agent and context statistics.
 *
 * Shows:
 * - Left: Thread ID/Title, agent status
 * - Center: Current provider/model, agent persona
 * - Right: Token usage, cost, compaction status
 */
class FooterBar {
public:
    explicit FooterBar(std::shared_ptr<AppState> state);
    ~FooterBar() = default;

    // FTXUI component interface
    ftxui::Element Render() const;

private:
    std::shared_ptr<AppState> state_;

    [[nodiscard]] std::string renderLeftSection() const;
    [[nodiscard]] std::string renderCenterSection() const;
    [[nodiscard]] std::string renderRightSection() const;
    [[nodiscard]] std::string statusToString(
        ::firmius::shared::AgentStatus status) const;
};

} // namespace firmius::tui

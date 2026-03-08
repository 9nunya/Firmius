#ifndef FIRMIUS_COMPONENTS_CHAT_WINDOW_HPP
#define FIRMIUS_COMPONENTS_CHAT_WINDOW_HPP

#include "Context.hpp"
#include "utils/ToolView.hpp"
#include <ftxui/component/component_base.hpp>
#include <functional>
#include <memory>

namespace firmius::tui {

using ToolViewProvider =
    std::function<std::shared_ptr<shared::ToolCallView>(const std::string &)>;

ftxui::Component ChatWindow(
    std::function<const shared::AgentHistory*()> history_getter,
    std::function<std::vector<ftxui::Element>()> live_rows_provider = nullptr,
    ToolViewProvider tool_view_provider = nullptr);

} // namespace firmius::tui

#endif

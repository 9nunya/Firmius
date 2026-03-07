#ifndef FIRMIUS_COMPONENTS_CHAT_WINDOW_HPP
#define FIRMIUS_COMPONENTS_CHAT_WINDOW_HPP

#include "Context.hpp"
#include <ftxui/component/component_base.hpp>
#include <functional>

namespace firmius::tui {

ftxui::Component ChatWindow(
    std::function<const shared::AgentHistory*()> history_getter,
    std::function<std::vector<ftxui::Element>()> live_rows_provider = nullptr);

} // namespace firmius::tui

#endif

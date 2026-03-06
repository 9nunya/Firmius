#ifndef FIRMIUS_COMPONENTS_CHAT_WINDOW_HPP
#define FIRMIUS_COMPONENTS_CHAT_WINDOW_HPP

#include "Context.hpp"
#include <ftxui/component/component_base.hpp>
namespace firmius::tui {

ftxui::Component ChatWindow(const std::shared_ptr<shared::AgentHistory>& history);

}

#endif

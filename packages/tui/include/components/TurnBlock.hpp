#ifndef FIRMIUS_COMPONENTS_TURN_BLOCK_HPP
#define FIRMIUS_COMPONENTS_TURN_BLOCK_HPP

#include "Context.hpp"
#include <ftxui/component/component_base.hpp>
namespace firmius::tui {

    ftxui::Component TurnBlock(const shared::AgentTurn& turn);

}

#endif

#ifndef FIRMIUS_COMPONENTS_TOOL_BLOCK_HPP
#define FIRMIUS_COMPONENTS_TOOL_BLOCK_HPP

#include <ftxui/component/component_base.hpp>
#include <memory>
#include <string>
#include <vector>

#include "utils/ToolView.hpp"

namespace firmius::tui {

using firmius::shared::ToolCallView;
using firmius::shared::ToolPhase;

ftxui::Component ToolBlock(const std::shared_ptr<ToolCallView> &view);

} // namespace firmius::tui

#endif

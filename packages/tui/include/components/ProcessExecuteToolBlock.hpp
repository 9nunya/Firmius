#ifndef FIRMIUS_TUI_COMPONENTS_PROCESS_EXECUTE_TOOL_BLOCK_HPP
#define FIRMIUS_TUI_COMPONENTS_PROCESS_EXECUTE_TOOL_BLOCK_HPP

#include "components/ToolBlock.hpp"
#include <ftxui/component/component.hpp>

namespace firmius::tui {
ftxui::Component
ProcessExecuteToolBlock(const std::shared_ptr<ToolCallView> &view);
}

#endif

#ifndef FIRMIUS_TUI_COMPONENTS_LIST_DIRECTORY_TOOL_BLOCK_HPP
#define FIRMIUS_TUI_COMPONENTS_LIST_DIRECTORY_TOOL_BLOCK_HPP

#include "components/ToolBlock.hpp"
#include <ftxui/component/component.hpp>

namespace firmius::tui {
ftxui::Component
ListDirectoryToolBlock(const std::shared_ptr<ToolCallView> &view);
}

#endif

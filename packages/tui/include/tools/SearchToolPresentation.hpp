#ifndef FIRMIUS_TUI_TOOLS_SEARCH_TOOL_PRESENTATION_HPP
#define FIRMIUS_TUI_TOOLS_SEARCH_TOOL_PRESENTATION_HPP

#include "tools/ToolPresentation.hpp"

namespace firmius::tui {

bool IsSearchFamilyTool(const std::string &tool_name);

ToolPresentation BuildSearchToolPresentation(const firmius::shared::ToolCallView &view);

} // namespace firmius::tui

#endif

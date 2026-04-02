#ifndef FIRMIUS_TUI_TOOLS_WEB_SEARCH_TOOL_PRESENTATION_HPP
#define FIRMIUS_TUI_TOOLS_WEB_SEARCH_TOOL_PRESENTATION_HPP

#include "tools/ToolPresentation.hpp"

namespace firmius::tui {

bool IsWebSearchFamilyTool(const std::string &tool_name);

ToolPresentation BuildWebSearchToolPresentation(const firmius::shared::ToolCallView &view);

} // namespace firmius::tui

#endif

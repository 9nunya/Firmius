#ifndef FIRMIUS_TUI_TOOLS_MCP_TOOL_PRESENTATION_HPP
#define FIRMIUS_TUI_TOOLS_MCP_TOOL_PRESENTATION_HPP

#include "tools/ToolPresentation.hpp"

namespace firmius::tui {

bool IsMcpFamilyTool(const std::string &tool_name);

ToolPresentation BuildMcpToolPresentation(const firmius::shared::ToolCallView &view);

} // namespace firmius::tui

#endif
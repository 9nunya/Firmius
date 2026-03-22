#ifndef FIRMIUS_TUI_TOOLS_WORK_TOOL_PRESENTATION_HPP
#define FIRMIUS_TUI_TOOLS_WORK_TOOL_PRESENTATION_HPP

#include "tools/ToolPresentation.hpp"
#include "utils/ToolView.hpp"

namespace firmius::tui {

bool IsWorkFamilyTool(const std::string &tool_name);

ToolPresentation BuildWorkToolPresentation(const firmius::shared::ToolCallView &view);

} // namespace firmius::tui

#endif

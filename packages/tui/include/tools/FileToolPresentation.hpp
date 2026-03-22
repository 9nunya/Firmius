#ifndef FIRMIUS_TUI_TOOLS_FILE_TOOL_PRESENTATION_HPP
#define FIRMIUS_TUI_TOOLS_FILE_TOOL_PRESENTATION_HPP

#include "tools/ToolPresentation.hpp"
#include "utils/ToolView.hpp"

namespace firmius::tui {

bool IsFileFamilyTool(const std::string &tool_name);

ToolPresentation BuildFileToolPresentation(const firmius::shared::ToolCallView &view);

} // namespace firmius::tui

#endif

#ifndef FIRMIUS_TUI_TOOLS_PROCESS_TOOL_PRESENTATION_HPP
#define FIRMIUS_TUI_TOOLS_PROCESS_TOOL_PRESENTATION_HPP

#include "tools/ToolPresentation.hpp"

namespace firmius::tui {

bool IsProcessFamilyTool(const std::string &tool_name);
ToolPresentation BuildProcessToolPresentation(
    const firmius::shared::ToolCallView &view,
    const NormalizedProcessState *process_state);

} // namespace firmius::tui

#endif

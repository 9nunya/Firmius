#ifndef FIRMIUS_TUI_TOOLS_SUBAGENT_TOOL_PRESENTATION_HPP
#define FIRMIUS_TUI_TOOLS_SUBAGENT_TOOL_PRESENTATION_HPP

#include "tools/SubagentState.hpp"
#include "tools/ToolPresentation.hpp"
#include "utils/ToolView.hpp"

namespace firmius::tui {

bool IsSubagentFamilyTool(const std::string &tool_name);
ToolPresentation BuildTerminateSubagentToolPresentation(
    const firmius::shared::ToolCallView &view);

ToolPresentation BuildSubagentToolPresentation(
    const firmius::shared::ToolCallView &view,
    const NormalizedSubagentState *subagent_state);

} // namespace firmius::tui

#endif

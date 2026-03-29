#ifndef FIRMIUS_TUI_TOOLS_PYTHON_TOOL_PRESENTATION_HPP
#define FIRMIUS_TUI_TOOLS_PYTHON_TOOL_PRESENTATION_HPP

#include "tools/ToolPresentation.hpp"
#include "tools/ProcessState.hpp"
#include "utils/ToolView.hpp"

namespace firmius::tui {

ToolPresentation BuildPythonToolPresentation(
    const firmius::shared::ToolCallView &view,
    const NormalizedProcessState *process_state);

} // namespace firmius::tui

#endif

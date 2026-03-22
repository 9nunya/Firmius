#ifndef FIRMIUS_TUI_TOOLS_ARTIFACT_TOOL_PRESENTATION_HPP
#define FIRMIUS_TUI_TOOLS_ARTIFACT_TOOL_PRESENTATION_HPP

#include "tools/ToolPresentation.hpp"
#include "utils/ToolView.hpp"

namespace firmius::tui {

bool IsArtifactFamilyTool(const std::string &tool_name);

ToolPresentation
BuildArtifactToolPresentation(const firmius::shared::ToolCallView &view);

} // namespace firmius::tui

#endif

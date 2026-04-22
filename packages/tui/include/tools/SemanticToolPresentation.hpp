#ifndef FIRMIUS_TUI_TOOLS_SEMANTIC_TOOL_PRESENTATION_HPP
#define FIRMIUS_TUI_TOOLS_SEMANTIC_TOOL_PRESENTATION_HPP

#include "tools/ToolPresentation.hpp"

namespace firmius::tui {

bool IsSemanticFamilyTool(const std::string &tool_name);

ToolPresentation BuildSemanticToolPresentation(const firmius::shared::ToolCallView &view);

} // namespace firmius::tui

#endif

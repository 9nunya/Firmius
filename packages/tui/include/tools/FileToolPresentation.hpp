#ifndef FIRMIUS_TUI_TOOLS_FILE_TOOL_PRESENTATION_HPP
#define FIRMIUS_TUI_TOOLS_FILE_TOOL_PRESENTATION_HPP

#include "tools/ToolPresentation.hpp"
#include "utils/ToolView.hpp"

namespace firmius::tui {

bool IsFileFamilyTool(const std::string &tool_name);
bool IsFileReadFamilyTool(const std::string &tool_name);
bool IsFileWriteFamilyTool(const std::string &tool_name);
bool IsDirectoryFamilyTool(const std::string &tool_name);

ToolPresentation BuildFileToolPresentation(const firmius::shared::ToolCallView &view);

} // namespace firmius::tui

#endif

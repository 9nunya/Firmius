#ifndef FIRMIUS_TUI_TOOLS_MODE_SWITCH_TOOL_PRESENTATION_HPP
#define FIRMIUS_TUI_TOOLS_MODE_SWITCH_TOOL_PRESENTATION_HPP

#include "tools/ToolPresentation.hpp"
#include "utils/ToolView.hpp"

namespace firmius::tui {

/**
 * @brief Returns true for the `ModeSwitch` tool family.
 *
 * Matches both the canonical name (`ModeSwitch`) and the snake-case
 * variant (`mode_switch`) so future renames don't silently fall through
 * to the generic JSON-blob renderer.
 */
bool IsModeSwitchTool(const std::string &tool_name);

/**
 * @brief Build a deliberately one-line presentation for a ModeSwitch
 * call.
 *
 * The full mode metadata (stance, allowed next modes, expected return
 * shape) is already injected into the next agent prompt by the tool
 * itself — there's no operator value in re-rendering it as a fact card.
 * What the operator wants is a quick mid-stream confirmation of the
 * stance flip, so we emit a single line: `from → to · stance`.
 */
ToolPresentation
BuildModeSwitchToolPresentation(const firmius::shared::ToolCallView &view);

} // namespace firmius::tui

#endif

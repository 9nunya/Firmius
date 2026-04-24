#ifndef FIRMIUS_COMPONENTS_TOOL_PRESENTATION_BLOCK_HPP
#define FIRMIUS_COMPONENTS_TOOL_PRESENTATION_BLOCK_HPP

#include "tools/ToolPresentation.hpp"
#include "utils/ToolView.hpp"
#include <ftxui/component/component_base.hpp>
#include <functional>
#include <memory>

namespace firmius::tui {

ftxui::Component ToolPresentationBlock(
    const std::shared_ptr<firmius::shared::ToolCallView> &view,
    std::function<ToolPresentation()> presentation_getter,
    std::function<bool()> compact_mode_getter = nullptr);

} // namespace firmius::tui

#endif

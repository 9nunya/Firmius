#ifndef FIRMIUS_TUI_COMPONENTS_SUBAGENT_TOOL_BLOCK_HPP
#define FIRMIUS_TUI_COMPONENTS_SUBAGENT_TOOL_BLOCK_HPP

#include "components/ToolBlock.hpp"
#include <ftxui/component/component.hpp>

namespace firmius::tui {
ftxui::Component SubagentToolBlock(const std::shared_ptr<ToolCallView> &view,
                                   HistoryGetter sub_history_getter = nullptr,
                                   StreamGetter sub_stream_getter = nullptr);
}

#endif

#ifndef FIRMIUS_COMPONENTS_TOOL_BLOCK_HPP
#define FIRMIUS_COMPONENTS_TOOL_BLOCK_HPP

#include <ftxui/component/component_base.hpp>
#include <memory>
#include <string>
#include <vector>

#include "utils/ToolView.hpp"

#include "StreamStateManager.hpp"

namespace firmius::tui {

using firmius::shared::ToolCallView;
using firmius::shared::ToolPhase;

using HistoryGetter =
    std::function<const firmius::shared::AgentHistory *(const std::string &)>;
using StreamGetter =
    std::function<const firmius::tui::StreamState *(const std::string &)>;

ftxui::Component ToolBlock(const std::shared_ptr<ToolCallView> &view,
                           HistoryGetter sub_history_getter = nullptr,
                           StreamGetter sub_stream_getter = nullptr);

} // namespace firmius::tui

#endif

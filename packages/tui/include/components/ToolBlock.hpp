#ifndef FIRMIUS_COMPONENTS_TOOL_BLOCK_HPP
#define FIRMIUS_COMPONENTS_TOOL_BLOCK_HPP

#include <ftxui/component/component_base.hpp>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "utils/ToolView.hpp"
#include "tools/ProcessState.hpp"
#include "tools/SubagentState.hpp"

#include "StreamStateManager.hpp"

namespace firmius::tui {

using firmius::shared::ToolCallView;
using firmius::shared::ToolPhase;

using HistoryGetter =
    std::function<const firmius::shared::AgentHistory *(const std::string &)>;
using StreamGetter =
    std::function<const firmius::tui::StreamState *(const std::string &)>;
using ProcessStateGetter =
    std::function<const firmius::tui::NormalizedProcessState *(const std::string &)>;
using SubagentStateGetter =
    std::function<const firmius::tui::NormalizedSubagentState *(const std::string &)>;
using AgentFocusHandler = std::function<void(const std::string &)>;

ftxui::Component ToolBlock(const std::shared_ptr<ToolCallView> &view,
                           HistoryGetter sub_history_getter = nullptr,
                           StreamGetter sub_stream_getter = nullptr,
                           ProcessStateGetter process_state_getter = nullptr,
                           SubagentStateGetter subagent_state_getter = nullptr,
                           AgentFocusHandler agent_focus_handler = nullptr);

} // namespace firmius::tui

#endif

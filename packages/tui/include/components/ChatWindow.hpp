#ifndef FIRMIUS_COMPONENTS_CHAT_WINDOW_HPP
#define FIRMIUS_COMPONENTS_CHAT_WINDOW_HPP

#include "Context.hpp"
#include "Message.hpp"
#include "components/ChatTranscriptPolicy.hpp"
#include "components/TranscriptGrouping.hpp"
#include "utils/ToolView.hpp"
#include "utils/StringUtil.hpp"
#include <ftxui/component/component_base.hpp>
#include <functional>
#include <memory>
#include <unordered_map>
#include <unordered_set>

#include "StreamStateManager.hpp"

namespace firmius::tui {

using ToolViewProvider =
    std::function<std::shared_ptr<shared::ToolCallView>(const std::string &)>;
using ProcessStateGetter =
    std::function<const NormalizedProcessState *(const std::string &)>;
using SubagentStateGetter =
    std::function<const NormalizedSubagentState *(const std::string &)>;
using AgentFocusHandler = std::function<void(const std::string &)>;

using HistoryGetter =
    std::function<const shared::AgentHistory *(const std::string &)>;
using StreamGetter = std::function<const StreamState *(const std::string &)>;
struct LiveQuickSummaryCluster {
  std::vector<QuickToolCategory> category_order;
  std::unordered_map<int, QuickToolGroupSummary> summaries;
  bool merge_with_history = true;
};
using LiveQuickSummaryProvider =
    std::function<std::vector<LiveQuickSummaryCluster>()>;
using EditableModeEnabledGetter = std::function<bool()>;
using EditableMessageSelectedGetter = std::function<bool(uint64_t)>;
using EditableMessageClickHandler = std::function<void(uint64_t)>;

ftxui::Component ChatWindow(
    std::function<const shared::AgentHistory *()> history_getter,
    std::function<std::vector<ftxui::Element>()> live_rows_provider = nullptr,
    ToolViewProvider tool_view_provider = nullptr,
    ProcessStateGetter process_state_getter = nullptr,
    SubagentStateGetter subagent_state_getter = nullptr,
    AgentFocusHandler agent_focus_handler = nullptr,
    HistoryGetter sub_history_getter = nullptr,
    StreamGetter sub_stream_getter = nullptr,
    LiveQuickSummaryProvider live_quick_summary_provider = nullptr,
    std::function<std::size_t()> live_measurement_signature_getter = nullptr,
    std::function<bool()> show_internal_nudges_getter = nullptr,
    std::function<bool()> hide_errors_getter = nullptr,
    std::function<bool()> show_turn_footers_getter = nullptr,
    EditableModeEnabledGetter editable_mode_enabled_getter = nullptr,
    EditableMessageSelectedGetter editable_message_selected_getter = nullptr,
    EditableMessageClickHandler editable_message_click_handler = nullptr);

} // namespace firmius::tui

#endif

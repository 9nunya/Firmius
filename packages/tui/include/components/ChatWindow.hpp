#ifndef FIRMIUS_COMPONENTS_CHAT_WINDOW_HPP
#define FIRMIUS_COMPONENTS_CHAT_WINDOW_HPP

#include "Context.hpp"
#include "Message.hpp"
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

inline bool ShouldRenderToolCallView(const shared::ToolCallView &view) {
  return shared::ToolCallHasRenderableIdentity(view);
}

inline std::unordered_set<std::string>
CollectToolCallIdsFromHistory(const shared::AgentHistory *history) {
  std::unordered_set<std::string> ids;
  if (!history) {
    return ids;
  }

  for (const auto &turn : history->turns) {
    for (const auto &msg : turn.messages) {
      for (const auto &part : msg.content) {
        if (auto *tc = std::get_if<shared::ToolCallContent>(&part)) {
          if (!tc->id.empty()) {
            ids.insert(tc->id);
          }
        } else if (auto *tr = std::get_if<shared::ToolResultContent>(&part)) {
          if (!tr->toolCallId.empty()) {
            ids.insert(tr->toolCallId);
          }
        }
      }
    }
  }

  return ids;
}

inline bool ShouldHideMessageInTranscript(const shared::Message &msg,
                                          bool showInternalNudges,
                                          const std::string &turnId = "") {
  if (msg.visibility == shared::MessageVisibility::Internal) {
    return !showInternalNudges;
  }
  if (msg.role != shared::Role::System) {
    return false;
  }
  if (turnId.rfind("compaction-start-", 0) == 0 ||
      turnId.rfind("compaction-summary-", 0) == 0 ||
      turnId.rfind("compaction-end-", 0) == 0 ||
      turnId.rfind("system-note-", 0) == 0) {
    return false;
  }
  for (const auto &part : msg.content) {
    if (const auto *notice = std::get_if<shared::NoticeContent>(&part)) {
      if (notice->title == "Agent Cancelled") {
        return true;
      }
      return false;
    }
    if (const auto *error = std::get_if<shared::ErrorContent>(&part)) {
      if (error->errorName == "Agent Cancelled") {
        return true;
      }
    }
  }
  return true;
}

inline ftxui::Element IndentAgentRow(const ftxui::Element &content,
                                     int left_margin = 2) {
  if (left_margin <= 0) {
    return content;
  }
  return ftxui::hbox({ftxui::text(std::string(left_margin, ' ')),
                      content | ftxui::xflex}) |
         ftxui::xflex;
}

inline bool ShouldRenderFocusedSubagentToolCall(
    const TimelineEntry &entry, const shared::ToolCallView &view,
    const std::string &focused_agent_id) {
  return !focused_agent_id.empty() && entry.agentId == focused_agent_id &&
         entry.kind == TimelineEntry::Kind::ToolCall &&
         ShouldRenderToolCallView(view);
}

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
    std::function<bool()> show_internal_nudges_getter = nullptr,
    std::function<bool()> hide_errors_getter = nullptr);

} // namespace firmius::tui

#endif

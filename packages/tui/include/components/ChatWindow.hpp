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

ftxui::Component ChatWindow(
    std::function<const shared::AgentHistory *()> history_getter,
    std::function<std::vector<ftxui::Element>()> live_rows_provider = nullptr,
    ToolViewProvider tool_view_provider = nullptr,
    HistoryGetter sub_history_getter = nullptr,
    StreamGetter sub_stream_getter = nullptr,
    LiveQuickSummaryProvider live_quick_summary_provider = nullptr);

} // namespace firmius::tui

#endif

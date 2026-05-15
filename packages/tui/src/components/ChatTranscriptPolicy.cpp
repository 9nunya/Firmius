#include "components/ChatTranscriptPolicy.hpp"

#include <ftxui/dom/elements.hpp>

#include <string>
#include <unordered_set>
#include <variant>

namespace firmius::tui {

bool ShouldRenderToolCallView(const shared::ToolCallView &view) {
  return shared::ToolCallHasRenderableIdentity(view);
}

std::unordered_set<std::string>
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

bool ShouldHideMessageInTranscript(const shared::Message &msg,
                                   bool showInternalNudges,
                                   const std::string &turnId) {
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

ftxui::Element IndentAgentRow(const ftxui::Element &content, int left_margin) {
  ftxui::Element safe = content ? content : ftxui::text("");
  if (left_margin <= 0) {
    return safe;
  }
  return ftxui::hbox({ftxui::text(std::string(left_margin, ' ')),
                      safe | ftxui::xflex}) |
         ftxui::xflex;
}

bool ShouldRenderFocusedSubagentToolCall(
    const TimelineEntry &entry, const shared::ToolCallView &view,
    const std::string &focused_agent_id) {
  return !focused_agent_id.empty() && entry.agentId == focused_agent_id &&
         entry.kind == TimelineEntry::Kind::ToolCall &&
         ShouldRenderToolCallView(view);
}

} // namespace firmius::tui

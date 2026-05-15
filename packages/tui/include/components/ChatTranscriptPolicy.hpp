#ifndef FIRMIUS_COMPONENTS_CHAT_TRANSCRIPT_POLICY_HPP
#define FIRMIUS_COMPONENTS_CHAT_TRANSCRIPT_POLICY_HPP

#include "Context.hpp"
#include "Message.hpp"
#include "StreamStateManager.hpp"
#include "components/TranscriptGrouping.hpp"
#include "utils/ToolView.hpp"

#include <ftxui/dom/elements.hpp>

#include <string>
#include <unordered_set>

namespace firmius::tui {

bool ShouldRenderToolCallView(const shared::ToolCallView &view);

std::unordered_set<std::string>
CollectToolCallIdsFromHistory(const shared::AgentHistory *history);

bool ShouldHideMessageInTranscript(const shared::Message &msg,
                                   bool showInternalNudges,
                                   const std::string &turnId = "");

ftxui::Element IndentAgentRow(const ftxui::Element &content,
                              int left_margin = 2);

bool ShouldRenderFocusedSubagentToolCall(
    const TimelineEntry &entry, const shared::ToolCallView &view,
    const std::string &focused_agent_id);

} // namespace firmius::tui

#endif

#ifndef FIRMIUS_TUI_UTILS_TRANSCRIPT_EXPANSION_HPP
#define FIRMIUS_TUI_UTILS_TRANSCRIPT_EXPANSION_HPP

#include "Events.hpp"

#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace firmius::core {
struct CompactionSnapshot;
}

namespace firmius::tui {
namespace shared = firmius::shared;

namespace detail {

std::optional<std::string>
compactionIdFromTurnIdForDisplay(const std::string &turnId);

std::vector<shared::AgentTurn> expandCompactionTranscriptTurnsForDisplay(
    const std::vector<shared::AgentTurn> &turns,
    const std::unordered_map<std::string, firmius::core::CompactionSnapshot>
        &snapshots,
    std::unordered_set<std::string> &expanded_ids);

inline bool shouldNotifyHiddenChatError(const std::string &focused_agent_id,
                                        const std::string &error_agent_id,
                                        bool hide_errors) {
  return hide_errors && !focused_agent_id.empty() &&
         error_agent_id == focused_agent_id;
}

} // namespace detail

std::vector<shared::AgentTurn> expandCompactionTranscriptForDisplay(
    const std::vector<shared::AgentTurn> &turns,
    const std::unordered_map<std::string, firmius::core::CompactionSnapshot>
        &snapshots);

} // namespace firmius::tui

#endif

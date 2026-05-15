#ifndef FIRMIUS_COMPONENTS_CHAT_MEASUREMENT_SIGNATURE_HPP
#define FIRMIUS_COMPONENTS_CHAT_MEASUREMENT_SIGNATURE_HPP

#include "StreamStateManager.hpp"

#include <cstddef>
#include <string>
#include <unordered_set>

namespace firmius::tui {

std::size_t BuildFocusedChatLiveMeasurementSignature(
    const StreamStateManager &stream_state, const std::string &focused_agent_id,
    const std::string &thread_id,
    const std::unordered_set<std::string> &persisted_tool_call_ids);

} // namespace firmius::tui

#endif

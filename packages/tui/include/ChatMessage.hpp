#pragma once

#include <string>
#include <vector>
#include <optional>
#include <cstdint>

#include "Message.hpp"
#include "Metrics.hpp"
#include "Enums.hpp"

namespace firmius::tui {

/**
 * @brief Enum for different types of blocks in the chat.
 */
enum class ChatMessageType {
    Normal,     ///< Standard message (User/Assistant/Tool)
    Compaction, ///< Context compacted notification
    Retry,      ///< Retrying notification
    Error,      ///< Error block
    Queued      ///< Queued tag for user messages
};

/**
 * @brief Wrapped message for the renderer and state.
 */
struct ChatMessage {
    ChatMessageType type = ChatMessageType::Normal;
    firmius::shared::Message message;
    std::string agentId;
    std::string friendlyName;
    std::optional<firmius::shared::AgentMetrics> metrics;
    
    // Metadata for special blocks
    std::string specialText;
    uint32_t tokensSaved = 0;
    int attempt = 0;
    int maxAttempts = 0;
    int delayMs = 0;
};

} // namespace firmius::tui

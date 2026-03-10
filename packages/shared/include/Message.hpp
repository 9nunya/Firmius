#ifndef FIRMIUS_SHARED_MESSAGE_HPP
#define FIRMIUS_SHARED_MESSAGE_HPP

#include "Enums.hpp"

#include <optional>
#include <string>
#include <variant>
#include <vector>

/**
 * @brief Conversation and message modeling.
 */
namespace firmius::shared {

/**
 * @brief Plain text message content.
 */
struct TextContent {
  std::string text;
  bool operator==(const TextContent &other) const = default;
};

/**
 * @brief Internal reasoning/thinking content from a model.
 */
struct ThinkingContent {
  std::string thinking;
  bool operator==(const ThinkingContent &other) const = default;
};

/**
 * @brief Representation of an outgoing tool call request.
 */
struct ToolCallContent {
  std::string id;   ///< Unique ID for the tool call.
  std::string name; ///< Name of the tool being called.
  std::string args; ///< Raw JSON arguments.
  bool operator==(const ToolCallContent &other) const = default;
};

/**
 * @brief Representation of an incoming tool execution result.
 */
struct ToolResultContent {
  std::string toolCallId; ///< ID of the tool call this result belongs to.
  std::string result;     ///< Raw JSON or text result.
  bool success;           ///< True if execution completed without error.
  std::string processId;
  std::string subagentId;
  bool operator==(const ToolResultContent &other) const = default;
};

/**
 * @brief Image content for multimodal messages.
 */
struct ImageContent {
  std::string url;       ///< Image URL or base64 data URI.
  std::string mediaType; ///< MIME type (e.g., "image/png", "image/jpeg").
  std::string detail;    ///< Vision detail level: "auto", "low", or "high".
  bool operator==(const ImageContent &) const = default;
};

/**
 * @brief Error content for agent or engine failures.
 */
struct ErrorContent {
  std::string errorName; ///< Short title of the error (e.g., "Provider Error").
  std::string description; ///< Brief description.
  std::string details;     ///< Comprehensive details or trace.
  bool operator==(const ErrorContent &other) const = default;
};

/**
 * @brief A variant representing any part of a complex multi-part message.
 */
using MessagePart = std::variant<TextContent, ThinkingContent, ToolCallContent,
                                 ToolResultContent, ImageContent, ErrorContent>;

/**
 * @brief A full conversation message.
 */
struct Message {
  std::string id;                   ///< Unique message identifier.
  Role role;                        ///< Role of the message sender.
  std::vector<MessagePart> content; ///< Multi-part content payload.
  std::uint64_t timestamp;          ///< Message creation timestamp.
  std::optional<std::string>
      parentId; ///< Optional ID of the parent message in a thread.

  bool operator==(const Message &other) const = default;
};

} // namespace firmius::shared

#endif

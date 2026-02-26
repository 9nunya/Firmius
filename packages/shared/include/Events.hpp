#ifndef FIRMIUS_SHARED_EVENTS_HPP
#define FIRMIUS_SHARED_EVENTS_HPP

#include "Metrics.hpp"

#include <string>
#include <variant>
#include <cstdint>

/**
 * @brief Real-time events emitted during agent execution.
 */
namespace firmius::shared {

/**
 * @brief A delta chunk of plain text content.
 */
struct TextChunk {
  std::string delta;

  bool operator==(const TextChunk& other) const {
    return delta == other.delta;
  }
};

/**
 * @brief A delta chunk of reasoning/thinking content.
 */
struct ThinkingChunk {
  std::string delta;

  bool operator==(const ThinkingChunk& other) const {
    return delta == other.delta;
  }
};

/**
 * @brief A delta chunk of a tool call request.
 */
struct ToolCallChunk {
  std::string id;           ///< Unique ID for the tool call (usually sent in first chunk).
  std::uint32_t index;      ///< Index of the tool call in parallel execution.
  std::string nameDelta;    ///< Partial name string.
  std::string argsDelta;    ///< Partial arguments JSON string.

  bool operator==(const ToolCallChunk& other) const {
    return id == other.id &&
           index == other.index &&
           nameDelta == other.nameDelta &&
           argsDelta == other.argsDelta;
  }
};

/**
 * @brief A variant representing any single event in an agent's output stream.
 */
using StreamEvent = std::variant<TextChunk, ThinkingChunk, ToolCallChunk, AgentMetrics>;

}

#endif

#ifndef FIRMIUS_SHARED_EVENTS_HPP
#define FIRMIUS_SHARED_EVENTS_HPP

#include "Metrics.hpp"
#include "Enums.hpp"
#include "Context.hpp"

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
 * @brief Terminal signal indicating the LLM stream has completed.
 */
struct StreamDone {
    StopReason reason;
    bool operator==(const StreamDone& other) const = default;
};

/**
 * @brief Terminal signal indicating the LLM stream failed.
 */
struct StreamError {
    std::string message;   ///< Human-readable error description.
    int httpStatus = 0;    ///< HTTP status code (0 if not an HTTP error).
    bool operator==(const StreamError& other) const = default;
};

/**
 * @brief Events emitted by the Engine regarding fleet orchestration.
 */
struct AgentSpawned { std::string agentId; std::string personaName; bool operator==(const AgentSpawned&) const = default; };
struct AgentThinking { std::string agentId; std::string delta; bool operator==(const AgentThinking&) const = default; };
struct AgentText { std::string agentId; std::string delta; bool operator==(const AgentText&) const = default; };
struct AgentToolCall { std::string agentId; std::string toolName; std::string toolArgs; bool operator==(const AgentToolCall&) const = default; };
struct AgentTurnCompleted { 
    std::string agentId; 
    AgentTurn turn;
    AgentMetrics aggregateMetrics;
    bool operator==(const AgentTurnCompleted&) const = default; 
};
struct AgentCompleted { std::string agentId; std::string summary; bool operator==(const AgentCompleted&) const = default; };
struct AgentError { std::string agentId; std::string message; bool operator==(const AgentError&) const = default; };
struct AgentCompacting { std::string agentId; bool operator==(const AgentCompacting&) const = default; };
struct ContextCompacted { std::string agentId; uint32_t tokensSaved; bool operator==(const ContextCompacted&) const = default; };

/**
 * @brief A variant representing any single event in an agent's output stream.
 */
using StreamEvent = std::variant<TextChunk, ThinkingChunk, ToolCallChunk, AgentMetrics, StreamDone, StreamError, AgentTurnCompleted, AgentCompacting, ContextCompacted>;

using EngineEvent = std::variant<AgentSpawned, AgentThinking, AgentText, AgentToolCall, AgentTurnCompleted, AgentCompleted, AgentError, AgentCompacting, ContextCompacted>;

}

#endif

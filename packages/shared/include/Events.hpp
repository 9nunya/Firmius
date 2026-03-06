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
 * @brief A delta chunk of a background process output.
 */
struct ProcessOutputDelta {
  std::string processId;
  std::string output;
  bool isStderr;
  bool finished;

  bool operator==(const ProcessOutputDelta& other) const = default;
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
 * @brief Emitted when a stream request is sent but no response has arrived.
 */
struct ProviderWaiting {
    bool operator==(const ProviderWaiting&) const = default;
};

/**
 * @brief Emitted when retrying a failed stream request.
 */
struct StreamRetrying {
    int attempt = 0;           ///< Current retry attempt (1-based).
    int maxAttempts = 0;       ///< Maximum retry attempts allowed.
    int httpStatus = 0;        ///< HTTP status code that triggered retry.
    int delayMs = 0;           ///< Delay before next attempt in milliseconds.
    std::string reason;        ///< Reason for retry (e.g., "rate limited").
    bool operator==(const StreamRetrying& other) const = default;
};

/**
 * @brief Emitted when all retry attempts have been exhausted.
 */
struct StreamRetryExhausted {
    int httpStatus = 0;        ///< Final HTTP status code.
    int attemptsMade = 0;      ///< Number of attempts made.
    std::string reason;        ///< Final error reason.
    bool operator==(const StreamRetryExhausted& other) const = default;
};

/**
 * @brief Events emitted by the Engine regarding fleet orchestration.
 */
struct AgentSpawned { std::string agentId; std::string personaName; std::string parentId; std::string friendlyName; std::string title; bool persistHistory; bool operator==(const AgentSpawned&) const = default; };

/**
 * @brief Emitted when an agent is waiting for a provider response.
 */
struct AgentProviderWaiting {
    std::string agentId;
    std::string parentId;
    bool operator==(const AgentProviderWaiting&) const = default;
};

/**
 * @brief Emitted when an agent's stream request is being retried.
 */
struct AgentRetrying {
    std::string agentId;       ///< Agent ID attempting the retry.
    int attempt = 0;           ///< Current retry attempt (1-based).
    int maxAttempts = 0;       ///< Maximum retry attempts allowed.
    int httpStatus = 0;        ///< HTTP status code that triggered retry.
    int delayMs = 0;           ///< Delay before next attempt in milliseconds.
    std::string reason;        ///< Reason for retry.
    std::string parentId;      ///< Parent agent ID.
    bool operator==(const AgentRetrying&) const = default;
};

/**
 * @brief Emitted when all retry attempts have failed.
 */
struct AgentRetryFailed {
    std::string agentId;       ///< Agent ID that failed.
    int httpStatus = 0;        ///< Final HTTP status code.
    std::string reason;        ///< Final error reason.
    std::string parentId;      ///< Parent agent ID.
    bool operator==(const AgentRetryFailed&) const = default;
};
struct AgentThinking { std::string agentId; std::string delta; std::string parentId; bool operator==(const AgentThinking&) const = default; };
struct AgentText { std::string agentId; std::string delta; std::string parentId; bool operator==(const AgentText&) const = default; };
struct AgentToolCall { std::string agentId; std::string toolCallId; std::string toolName; std::string toolArgs; std::string parentId; bool operator==(const AgentToolCall&) const = default; };
struct AgentToolCallChunk { std::uint32_t index; std::string agentId; std::string toolCallId; std::string nameDelta; std::string argsDelta; std::string parentId; bool operator==(const AgentToolCallChunk&) const = default; };
struct AgentTurnCompleted { 
    std::string agentId; 
    AgentTurn turn;
    AgentMetrics aggregateMetrics;
    std::string parentId;
    bool operator==(const AgentTurnCompleted&) const = default; 
};
struct AgentCompleted { std::string agentId; std::string summary; std::string parentId; bool operator==(const AgentCompleted&) const = default; };
struct AgentError { std::string agentId; std::string message; std::string parentId; bool operator==(const AgentError&) const = default; };
struct AgentCompacting { std::string agentId; std::string parentId; bool operator==(const AgentCompacting&) const = default; };
struct AgentCompactionThinking { std::string agentId; std::string delta; std::string parentId; bool operator==(const AgentCompactionThinking&) const = default; };
struct AgentCompactionText { std::string agentId; std::string delta; std::string parentId; bool operator==(const AgentCompactionText&) const = default; };
struct ContextCompacted { std::string agentId; uint32_t tokensSaved; std::string parentId; bool operator==(const ContextCompacted&) const = default; };
struct AgentProcessOutput { std::string agentId; std::string processId; std::string output; bool isStderr; bool finished; std::string parentId; bool operator==(const AgentProcessOutput&) const = default; };
struct ModelSwitched {
    std::string agentId;
    std::string oldProviderId;
    std::string oldModelId;
    std::string newProviderId;
    std::string newModelId;
    std::string parentId;
    bool operator==(const ModelSwitched& other) const = default;
};

/**
 * @brief Emitted when agent history is undone.
 */
struct HistoryUndone {
    std::string agentId;
    std::string threadId;
    int turnsRemoved;
    bool compactionReversed;
    std::string parentId;
    bool operator==(const HistoryUndone& other) const = default;
};

/**
 * @brief A variant representing any single event in an agent's output stream.
 */
using StreamEvent = std::variant<
  TextChunk,
  ThinkingChunk,
  ToolCallChunk,
  AgentMetrics,
  StreamDone,
  StreamError,
  ProviderWaiting,
  StreamRetrying,
  StreamRetryExhausted,
  AgentTurnCompleted,
  AgentCompacting,
  AgentCompactionThinking,
  AgentCompactionText,
  ContextCompacted,
  ProcessOutputDelta
>;

using EngineEvent = std::variant<
  AgentSpawned,
  AgentProviderWaiting,
  AgentRetrying,
  AgentRetryFailed,
  AgentThinking,
  AgentText,
  AgentToolCall,
  AgentToolCallChunk,
  AgentTurnCompleted,
  AgentCompleted,
  AgentError,
  AgentCompacting,
  AgentCompactionThinking,
  AgentCompactionText,
  ContextCompacted,
  AgentProcessOutput,
  ModelSwitched,
  HistoryUndone
>;

}

#endif

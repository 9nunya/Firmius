#pragma once

#include "Context.hpp"
#include "Message.hpp"
#include "Metrics.hpp"
#include <string>
#include <variant>

namespace firmius::harness {

using namespace firmius::shared;

/**
 * Emitted when the current thread changes.
 */
struct ThreadChanged {
  std::string threadId;
  ThreadMetadata metadata;
};

/**
 * Emitted when a stream of text arrives from an agent.
 */
struct MessageChunk {
  std::string agentId;
  std::string delta;
  bool isThinking;
};

/**
 * @brief Emitted when an agent is waiting for a provider response.
 */
struct AgentProviderWaiting {
  std::string agentId;
};

/**
 * @brief Emitted when an agent completes a full message.
 */
struct MessageCompleted {
  std::string agentId;
  Message message;
  AgentMetrics metrics;
};

/**
 * Emitted when an agent starts a tool call.
 */
struct ToolCallStarted {
  std::string agentId;
  std::string toolCallId;
  std::string name;
  std::string args;
};

/**
 * Emitted when a tool call completes.
 */
struct ToolCallResult {
  std::string agentId;
  std::string toolCallId;
  std::string result;
  bool success;
  std::string processId;
  std::string subagentId;
};

/**
 * Emitted when a subagent is spawned.
 */
struct SubagentSpawned {
  std::string parentId;
  std::string agentId;
  std::string persona;
  std::string friendlyName;
  std::string title;
  std::string providerId;
  std::string modelId;
  uint32_t maxTokens = 0;
};

/**
 * Emitted when a background process outputs data.
 */
struct ProcessOutputChunk {
  std::string agentId;
  std::string pid;
  std::string delta;
  bool isStderr;
};

/**
 * Emitted when an agent spawns a background process.
 */
struct AgentProcessSpawned {
  std::string agentId;
  std::string processId;
  std::string toolCallId;
  std::string command;
};

/**
 * Emitted when a thread cannot be locked (already in use by another process).
 */
struct ThreadLocked {
  std::string threadId;
  int ownerPid;
};

/**
 * Emitted when a harness error occurs.
 */
struct HarnessError {
  std::string message;
};

/**
 * Emitted when an agent starts context compaction.
 */
struct AgentCompactingEvent {
  std::string agentId;
};

/**
 * Emitted when context compaction completes.
 */
struct ContextCompactedEvent {
  std::string agentId;
  uint32_t tokensSaved;
};

/**
 * Emitted when a thread is deleted.
 */
struct ThreadDeleted {
  std::string threadId;
};

/**
 * Emitted when the user configuration is updated.
 */
struct ConfigUpdated {};

/**
 * Emitted when an agent's model is switched.
 */
struct ModelSwitchedEvent {
  std::string agentId;
  std::string oldProviderId;
  std::string oldModelId;
  std::string newProviderId;
  std::string newModelId;
  uint32_t newMaxTokens = 0;
};

/**
 * Emitted when agent history is undone.
 */
struct HistoryUndoneEvent {
  std::string threadId;
  std::string agentId;
  int turnsRemoved;
  bool compactionReversed;
};

/**
 * Emitted when a thread's title is updated.
 */
struct ThreadTitleUpdated {
  std::string threadId;
  std::string title;
};

/**
 * Emitted when a message is queued while agent is running.
 */
struct MessageQueued {
  std::string messageId;
  std::string text;
};

/**
 * Emitted when a queued message is dequeued and sent.
 */
struct MessageDequeued {
  std::string messageId;
};

/**
 * Emitted when an agent is retrying a failed request.
 */
struct AgentRetrying {
  std::string agentId; ///< Agent ID attempting the retry.
  int attempt = 0;     ///< Current retry attempt (1-based).
  int maxAttempts = 0; ///< Maximum retry attempts allowed.
  int httpStatus = 0;  ///< HTTP status code that triggered retry.
  int delayMs = 0;     ///< Delay before next attempt in milliseconds.
  std::string reason;  ///< Reason for retry.
};

/**
 * Emitted when all retry attempts have failed.
 */
struct AgentRetryFailed {
  std::string agentId; ///< Agent ID that failed.
  int httpStatus = 0;  ///< Final HTTP status code.
  std::string reason;  ///< Final error reason.
};

/**
 * Emitted when the user sends a message.
 */
struct UserMessageSent {
  std::string messageId;
  std::string text;
  std::string threadId;
};

struct AgentFinished {
  std::string agentId;
};

/**
 * Emitted when a chunk of tool arguments arrives.
 */
struct ToolCallArgsChunk {
  std::string agentId;
  std::string toolCallId;
  std::string nameDelta;
  std::string delta;
};

/**
 * Emitted when a chunk of compaction thinking arrives.
 */
struct CompactionThinkingChunk {
  std::string agentId;
  std::string delta;
};

/**
 * Emitted when a chunk of compaction text arrives.
 */
struct CompactionTextChunk {
  std::string agentId;
  std::string delta;
};

/**
 * HarnessEvent is a variant of all events that the Harness can emit.
 */
using HarnessEvent = std::variant<
    ThreadChanged, AgentProviderWaiting, MessageChunk, MessageCompleted,
    ToolCallStarted, ToolCallResult, SubagentSpawned, ProcessOutputChunk,
    ThreadLocked, HarnessError, AgentCompactingEvent, ContextCompactedEvent,
    ThreadDeleted, ConfigUpdated, ModelSwitchedEvent, HistoryUndoneEvent,
    ThreadTitleUpdated, MessageQueued, MessageDequeued, AgentRetrying,
    AgentRetryFailed, UserMessageSent, AgentFinished, ToolCallArgsChunk,
    CompactionThinkingChunk, CompactionTextChunk, AgentProcessSpawned>;

} // namespace firmius::harness

#ifndef FIRMIUS_SHARED_EVENTS_HPP
#define FIRMIUS_SHARED_EVENTS_HPP

#include "Context.hpp"
#include "Enums.hpp"
#include "ICommandIntent.hpp"
#include "Metrics.hpp"

#include <cstdint>
#include <limits>
#include <string>
#include <utility>
#include <variant>
#include <vector>

/**
 * @brief Real-time events emitted during agent execution.
 */
namespace firmius::shared {

/**
 * @brief A delta chunk of plain text content.
 */
struct TextChunk {
  std::string delta;

  bool operator==(const TextChunk &other) const { return delta == other.delta; }
};

/**
 * @brief A delta chunk of reasoning/thinking content.
 */
struct ThinkingChunk {
  std::string delta;
  std::string signature;

  bool operator==(const ThinkingChunk &other) const {
    return delta == other.delta && signature == other.signature;
  }
};

/**
 * @brief A delta chunk of a tool call request.
 */
struct ToolCallChunk {
  std::string
      id; ///< Unique ID for the tool call (usually sent in first chunk).
  std::uint32_t index =
      std::numeric_limits<std::uint32_t>::max(); ///< Index of the tool call in
                                                  ///< parallel execution.
  std::string nameDelta; ///< Partial name string.
  std::string argsDelta; ///< Partial arguments JSON string.

  bool operator==(const ToolCallChunk &other) const {
    return id == other.id && index == other.index &&
           nameDelta == other.nameDelta && argsDelta == other.argsDelta;
  }
};

/**
 * @brief A finalized tool call request with a complete executable payload.
 */
struct ToolCall {
  std::string id;
  std::uint32_t index =
      std::numeric_limits<std::uint32_t>::max();
  std::string name;
  std::string args;

  bool operator==(const ToolCall &other) const {
    return id == other.id && index == other.index && name == other.name &&
           args == other.args;
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
  int exitCode = -1;
  double durationMs = 0.0;

  bool operator==(const ProcessOutputDelta &other) const = default;
};

/**
 * @brief Terminal signal indicating the LLM stream has completed.
 */
struct StreamDone {
  StopReason reason;
  bool operator==(const StreamDone &other) const = default;
};

/**
 * @brief Terminal signal indicating the LLM stream failed.
 */
struct StreamError {
  std::string message; ///< Human-readable error description.
  int httpStatus = 0;  ///< HTTP status code (0 if not an HTTP error).
  std::string
      accountLocator; ///< Account identifier/email that produced the error.
  bool operator==(const StreamError &other) const = default;
};

/**
 * @brief Emitted when a stream request is sent but no response has arrived.
 */
struct ProviderWaiting {
  bool operator==(const ProviderWaiting &) const = default;
};

/**
 * @brief Emitted when retrying a failed stream request.
 */
struct StreamRetrying {
  int attempt = 0;            ///< Current retry attempt (1-based).
  int maxAttempts = 0;        ///< Maximum retry attempts allowed.
  int httpStatus = 0;         ///< HTTP status code that triggered retry.
  int delayMs = 0;            ///< Delay before next attempt in milliseconds.
  std::string reason;         ///< Reason for retry (e.g., "rate limited").
  std::string accountLocator; ///< Account identifier/email attempting retry.
  std::string details;        ///< Optional structured retry details.
  bool operator==(const StreamRetrying &other) const = default;
};

/**
 * @brief Emitted when the provider switches accounts (e.g., OAuth account
 * rotation).
 */
struct StreamAccountSwitched {
  std::string accountLocator; ///< The new account identifier/email being used.
  bool operator==(const StreamAccountSwitched &other) const = default;
};

/**
 * @brief Emitted when all retry attempts have been exhausted.
 */
struct StreamRetryExhausted {
  int httpStatus = 0;   ///< Final HTTP status code.
  int attemptsMade = 0; ///< Number of attempts made.
  std::string reason;   ///< Final error reason.
  bool operator==(const StreamRetryExhausted &other) const = default;
};

/**
 * @brief Events emitted by the Engine regarding fleet orchestration.
 */
struct AgentSpawned {
  std::string agentId;
  std::string personaName;
  std::string parentId;
  std::string friendlyName;
  std::string title;
  bool persistHistory = true;
  std::string providerId = "";
  std::string modelId = "";
  uint32_t maxTokens = 0;
  bool operator==(const AgentSpawned &) const = default;
};

/**
 * @brief Emitted when an agent is waiting for a provider response.
 */
struct AgentProviderWaiting {
  std::string agentId;
  std::string parentId;
  bool operator==(const AgentProviderWaiting &) const = default;
};

/**
 * @brief Emitted when an agent's stream request is being retried.
 */
struct AgentRetrying {
  std::string agentId;        ///< Agent ID attempting the retry.
  int attempt = 0;            ///< Current retry attempt (1-based).
  int maxAttempts = 0;        ///< Maximum retry attempts allowed.
  int httpStatus = 0;         ///< HTTP status code that triggered retry.
  int delayMs = 0;            ///< Delay before next attempt in milliseconds.
  std::string reason;         ///< Reason for retry.
  std::string parentId;       ///< Parent agent ID.
  std::string accountLocator; ///< Account identifier/email attempting retry.
  std::string details;        ///< Optional structured retry details.
  bool operator==(const AgentRetrying &) const = default;
};

/**
 * @brief Emitted when an agent's provider switches accounts.
 */
struct AgentAccountSwitched {
  std::string agentId;        ///< Agent ID whose stream switched accounts.
  std::string accountLocator; ///< The new account identifier/email being used.
  std::string parentId;       ///< Parent agent ID.
  bool operator==(const AgentAccountSwitched &) const = default;
};

/**
 * @brief Emitted when all retry attempts have failed.
 */
struct AgentRetryFailed {
  std::string agentId;  ///< Agent ID that failed.
  int httpStatus = 0;   ///< Final HTTP status code.
  std::string reason;   ///< Final error reason.
  std::string parentId; ///< Parent agent ID.
  bool operator==(const AgentRetryFailed &) const = default;
};

struct AgentThinking {
  std::string agentId;
  std::string delta;
  std::string parentId = "";
  bool operator==(const AgentThinking &) const = default;
};

struct AgentText {
  std::string agentId;
  std::string delta;
  std::string parentId = "";
  bool operator==(const AgentText &) const = default;
};

struct AgentToolCall {
  std::string agentId;
  std::string toolCallId;
  std::string toolName;
  std::string toolArgs;
  std::string parentId = "";
  bool operator==(const AgentToolCall &) const = default;
};

struct AgentToolCallChunk {
  std::uint32_t index;
  std::string agentId;
  std::string toolCallId;
  std::string nameDelta;
  std::string argsDelta;
  std::string parentId = "";
  bool operator==(const AgentToolCallChunk &) const = default;
};

struct AgentFileEdited {
  std::string agentId;
  std::string parentId = "";
  std::string path;
  std::string toolCallId;
  std::string diffPreview;
  int addedLines = 0;
  int removedLines = 0;
  bool operator==(const AgentFileEdited &) const = default;
};

struct AgentTurnCompleted {
  std::string agentId;
  AgentTurn turn;
  AgentMetrics aggregateMetrics;
  std::string parentId = "";
  bool operator==(const AgentTurnCompleted &) const = default;
};

struct AgentMetricsStreamed {
  std::string agentId;
  AgentMetrics metrics;
  std::string parentId = "";
  bool operator==(const AgentMetricsStreamed &) const = default;
};

struct AgentOutcome {
  enum class Kind {
    Response,
    NoSummary,
    Cancelled,
    Failed,
  };

  Kind kind = Kind::Failed;
  std::string text;
  std::vector<ThreadArtifactMetadata> artifacts_created;
  std::vector<ThreadArtifactMetadata> artifacts_updated;

  AgentOutcome() = default;
  AgentOutcome(Kind kindValue, std::string textValue)
      : kind(kindValue), text(std::move(textValue)) {}
  AgentOutcome(Kind kindValue, std::string textValue,
               std::vector<ThreadArtifactMetadata> created,
               std::vector<ThreadArtifactMetadata> updated)
      : kind(kindValue), text(std::move(textValue)),
        artifacts_created(std::move(created)),
        artifacts_updated(std::move(updated)) {}

  bool operator==(const AgentOutcome &) const = default;
};

struct AgentInterrupted {
  std::string agentId;
  std::string parentId = "";
  bool operator==(const AgentInterrupted &) const = default;
};

struct AgentError {
  std::string agentId;
  std::string message;
  std::string parentId = "";
  bool operator==(const AgentError &) const = default;
};

struct AgentCompacting {
  std::string agentId;
  std::string parentId = "";
  bool operator==(const AgentCompacting &) const = default;
};

struct AgentCompactionThinking {
  std::string agentId;
  std::string delta;
  std::string parentId = "";
  bool operator==(const AgentCompactionThinking &) const = default;
};

struct AgentCompactionText {
  std::string agentId;
  std::string delta;
  std::string parentId = "";
  bool operator==(const AgentCompactionText &) const = default;
};

struct ContextCompacted {
  std::string agentId;
  uint32_t tokensSaved;
  std::string parentId = "";
  bool operator==(const ContextCompacted &) const = default;
};

struct AgentProcessOutput {
  std::string agentId;
  std::string processId;
  std::string output;
  bool isStderr;
  bool finished;
  int exitCode = -1;
  double durationMs = 0.0;
  std::string parentId = "";
  bool operator==(const AgentProcessOutput &) const = default;
};

struct AgentProcessSpawned {
  std::string agentId;
  std::string processId;
  std::string toolCallId;
  std::string command;
  std::string parentId = "";
  bool operator==(const AgentProcessSpawned &) const = default;
};

struct ModelSwitched {
  std::string agentId;
  std::string oldProviderId;
  std::string oldModelId;
  std::string newProviderId;
  std::string newModelId;
  std::string parentId = "";
  bool operator==(const ModelSwitched &other) const = default;
};

struct HistoryUndone {
  std::string agentId;
  std::string threadId;
  int turnsRemoved;
  bool compactionReversed;
  std::string parentId = "";
  bool operator==(const HistoryUndone &other) const = default;
};

struct ThreadChanged {
  std::string threadId;
  ThreadMetadata metadata;
  bool operator==(const ThreadChanged &) const = default;
};

struct ThreadMetadataUpdated {
  std::string threadId;
  ThreadMetadata metadata;
  bool operator==(const ThreadMetadataUpdated &) const = default;
};

/**
 * @brief Emitted when core needs the UI to resolve a permission escalation.
 */
struct PermissionEscalationRequest {
  std::string requestId;
  std::string threadId;
  std::string agentId;
  PermissionRequestType requestType = PermissionRequestType::Read;
  std::string title;
  std::string message;
  std::string command;
  CommandSeverity severity = CommandSeverity::MEDIUM;
  std::string targetPath;
  std::string toolName;
  std::string toolCallId;
  std::string commandPrimary;
  bool allowAlways = true;
  bool isDirectory = false;
  bool operator==(const PermissionEscalationRequest &) const = default;
};

/**
 * @brief Emitted when a pending permission escalation is answered.
 */
struct PermissionEscalationResolved {
  std::string requestId;
  std::string threadId;
  std::string agentId;
  PermissionResponse response = PermissionResponse::Deny;
  bool operator==(const PermissionEscalationResolved &) const = default;
};

/**
 * @brief Emitted when a thread cannot be locked (already in use by another
 * process).
 */
struct ThreadLocked {
  std::string threadId;
  int ownerPid;
  bool operator==(const ThreadLocked &) const = default;
};

/**
 * @brief Emitted when a thread is deleted.
 */
struct ThreadDeleted {
  std::string threadId;
  bool operator==(const ThreadDeleted &) const = default;
};

/**
 * @brief Emitted when the user configuration is updated.
 */
struct ConfigUpdated {
  bool operator==(const ConfigUpdated &) const = default;
};

/**
 * @brief Emitted when the model cache has been refreshed.
 */
struct ModelsRefreshed {
  bool operator==(const ModelsRefreshed &) const = default;
};

/**
 * @brief Emitted when a provider starts fetching its model catalog.
 */
struct ProviderModelsFetchStarted {
  std::string providerId;
  bool operator==(const ProviderModelsFetchStarted &) const = default;
};

/**
 * @brief Emitted when a provider finishes fetching its model catalog.
 */
struct ProviderModelsFetchFinished {
  std::string providerId;
  std::string error;
  bool operator==(const ProviderModelsFetchFinished &) const = default;
};

/**
 * @brief Emitted when a single model is discovered and added to cache.
 */
struct ModelDiscovered {
  ModelInfo model;
  bool operator==(const ModelDiscovered &) const = default;
};

/**
 * @brief Emitted when a thread's title is updated.
 */
struct ThreadTitleUpdated {
  std::string threadId;
  std::string title;
  bool operator==(const ThreadTitleUpdated &) const = default;
};

/**
 * @brief Emitted when a message is queued while agent is running.
 */
struct MessageQueued {
  std::string messageId;
  std::string text;
  std::string threadId;
  std::string agentId;
  std::vector<firmius::shared::ImageContent> images;
  bool operator==(const MessageQueued &) const = default;
};

/**
 * @brief Emitted when a queued message is dequeued and sent.
 */
struct MessageDequeued {
  std::string messageId;
  std::string threadId;
  std::string agentId;
  bool operator==(const MessageDequeued &) const = default;
};

/**
 * @brief Emitted when an internal nudge message is queued while agent is running.
 */
struct InternalMessageQueued {
  std::string messageId;
  std::string text;
  std::string threadId;
  std::string agentId;
  bool operator==(const InternalMessageQueued &) const = default;
};

/**
 * @brief Emitted when a queued internal message is dequeued and sent.
 */
struct InternalMessageDequeued {
  std::string messageId;
  std::string threadId;
  std::string agentId;
  bool operator==(const InternalMessageDequeued &) const = default;
};

/**
 * @brief Emitted when the user sends a message.
 */
struct UserMessageSent {
  std::string messageId;
  std::string text;
  std::string threadId;
  std::string agentId;
  std::vector<firmius::shared::ImageContent> images;
  bool operator==(const UserMessageSent &) const = default;
};

/**
 * @brief Emitted when an agent's persisted todo list changes.
 */
struct AgentTodoUpdated {
  std::string threadId;
  std::string agentId;
  AgentTodoList todo;
  bool operator==(const AgentTodoUpdated &) const = default;
};

/**
 * @brief Emitted when an agent execution session finishes.
 */
struct AgentFinished {
  std::string agentId;
  AgentOutcome outcome;
  std::string parentId = "";
  bool operator==(const AgentFinished &) const = default;
};

/**
 * @brief Emitted when an embedding model download progresses.
 */
struct EmbeddingModelProgress {
  std::string agentId;
  std::string parentId = "";
  std::string modelId;
  uint64_t bytesDownloaded = 0;
  uint64_t totalBytes = 0;
  std::string status; // "downloading", "extracting", "ready", "error"
  bool operator==(const EmbeddingModelProgress &) const = default;
};

/**
 * @brief A variant representing any single event in an agent's output stream.
 */
using StreamEvent =
    std::variant<TextChunk, ThinkingChunk, ToolCallChunk, ToolCall, AgentMetrics,
                 StreamDone, StreamError, ProviderWaiting, StreamRetrying,
                 StreamRetryExhausted, StreamAccountSwitched,
                 AgentFileEdited, AgentTurnCompleted, AgentCompacting,
                 AgentCompactionThinking, AgentCompactionText, ContextCompacted,
                 ProcessOutputDelta, AgentProcessSpawned>;

using EngineEvent =
    std::variant<AgentSpawned, AgentProviderWaiting, AgentRetrying,
                 AgentRetryFailed, AgentThinking, AgentText, AgentToolCall,
                 AgentToolCallChunk, AgentFileEdited, AgentTurnCompleted,
                 AgentMetricsStreamed,
                 AgentInterrupted, AgentError, AgentCompacting,
                 AgentCompactionThinking, AgentCompactionText, ContextCompacted,
                 AgentProcessOutput, AgentProcessSpawned, ModelSwitched,
                 HistoryUndone, AgentAccountSwitched, AgentFinished>;

/**
 * @brief Unified event type for the entire application.
 */
using AppEvent = std::variant<
    AgentSpawned, AgentProviderWaiting, AgentRetrying, AgentRetryFailed,
    AgentThinking, AgentText, AgentToolCall, AgentToolCallChunk, AgentFileEdited,
    AgentTurnCompleted, AgentMetricsStreamed, AgentInterrupted, AgentError, AgentCompacting,
    AgentCompactionThinking, AgentCompactionText, ContextCompacted,
    AgentProcessOutput, AgentProcessSpawned, ModelSwitched, HistoryUndone,
    AgentAccountSwitched, ThreadChanged, ThreadMetadataUpdated,
    PermissionEscalationRequest, PermissionEscalationResolved, ThreadLocked,
    ThreadDeleted, ConfigUpdated, ModelsRefreshed, ProviderModelsFetchStarted,
    ProviderModelsFetchFinished, ModelDiscovered, ThreadTitleUpdated,
    MessageQueued, MessageDequeued, InternalMessageQueued,
    InternalMessageDequeued, UserMessageSent, AgentTodoUpdated, AgentFinished,
    EmbeddingModelProgress>;

} // namespace firmius::shared

#endif

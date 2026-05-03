#ifndef FIRMIUS_SHARED_CONTEXT_HPP
#define FIRMIUS_SHARED_CONTEXT_HPP

#include "Enums.hpp"
#include "Message.hpp"
#include "Metrics.hpp"

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

/**
 * @brief State and environment context for the agent engine.
 */
namespace firmius::shared {

/**
 * @brief Identity and personality metadata for an agent.
 */
struct AgentIdentity {
  std::string id;           ///< Unique identity ID.
  std::string name;         ///< Display name of the agent.
  std::string role;         ///< Assigned role (e.g., "coder").
  std::string goal;         ///< High-level mission goal.
  std::string systemPrompt; ///< Raw persona instructions.
  std::string parentId;     ///< ID of the parent agent (empty for root agents).
  std::string friendlyName; ///< Machine-friendly slug name (e.g., "aster",
                            ///< "auth-finder").

  bool operator==(const AgentIdentity &other) const = default;
};

/**
 * @brief Sandbox permission set for an agent.
 */
struct AgentPermissions {
  std::vector<ToolScope>
      allowedScopes; ///< Tools the agent is allowed to invoke.
  std::vector<std::string>
      allowedPaths;             ///< Filesystem paths the agent can access.
  bool allowOutsideCwd = false; ///< If true, allows access beyond /work.

  bool operator==(const AgentPermissions &other) const = default;
};

/**
 * @brief Host environment details.
 */
struct HostEnvironment {
  HostType type;          ///< Environment type (Local/Docker).
  std::string identifier; ///< Host identifier (e.g., container ID).
  std::string cwd;        ///< Current working directory.
  std::map<std::string, std::string> envVars; ///< Environment variables.

  bool operator==(const HostEnvironment &other) const = default;
};

/**
 * @brief Real-time runtime state of the agent.
 */
struct AgentState {
  AgentStatus currentStatus = AgentStatus::Idle; ///< Current lifecycle status.
  std::vector<std::string>
      pendingToolCalls; ///< IDs of tools currently executing.
  std::vector<std::string>
      ownedProcesses; ///< IDs of spawned background processes.
  std::vector<std::string> loadedSkills; // SKILL IDs of loaded skills.
  std::vector<std::string>
      loadedAgentMds;                 // AGENT.md paths that are loaded, broh
  std::map<std::string, std::string> loadedSkillRoots; // Path -> SkillRoot mapping.
  std::vector<std::string> readFiles; ///< Paths of files read in this session.
  std::vector<std::string> loadedMcpServers; // MCP server names with loaded selections.
  std::map<std::string, std::vector<std::string>> loadedMcpTools; // server -> loaded tool names.
  struct DynamicMcpTool {
    std::string name;
    std::string description;
    std::string inputSchema;

    bool operator==(const DynamicMcpTool &other) const = default;
  };
  std::map<std::string, std::vector<DynamicMcpTool>> loadedMcpToolDefinitions; // server -> loaded tool defs.
  std::map<std::string, std::vector<std::string>> loadedMcpResources; // server -> loaded resource URIs.
  std::map<std::string, std::vector<std::string>> loadedMcpPrompts; // server -> loaded prompt names.
  std::vector<std::string>
      fullyReadFiles; ///< Set of paths that were read entirely
  std::vector<std::string>
      editedFiles; ///< Paths of files successfully edited or written.
  std::vector<std::string>
      completedActions; ///< High-level actions completed (e.g., "Applied null
                        ///< check to _cull").
  std::optional<std::string>
      fatalError; ///< Description of fatal failure, if any.
  std::vector<std::string>
      blockingProcessIds; ///< IDs of background processes currently blocking
                          ///< agent execution.
  std::vector<std::string>
      recentToolCallSignatures; ///< Hash of "toolName:args" for recent calls
                                ///< (insanity loop detection)

  /// Currently active mode (qualified name, e.g. "diagnose" or "forge:apply").
  /// Empty when no mode is active. Mutated by the mode_switch tool;
  /// read by the prompt composer (ModeOverlaySection / ModeFrameSection)
  /// and by the TUI status zone.
  std::string activeMode;

  /// Initial mode the user picked on the welcome screen, before any tool
  /// could mutate it. Used so the welcome screen / status band can show
  /// "started in: forge:prime" alongside the live activeMode.
  std::string initialMode;

  bool operator==(const AgentState &other) const = default;
};

/**
 * @brief Generation and lifecycle configuration for an agent.
 */
struct AgentConfig {
  struct RollingModelConfig {
    bool enabled = false;
    std::string providerId;
    std::string modelId;
    std::string variantName;

    bool operator==(const RollingModelConfig &other) const = default;
  };

  struct RollingMemoryConfig {
    bool enabled = true;
    std::string mode = "rolling_forever";
    std::string preset = "balanced";
    float targetOccupancyRatio = 0.57f;
    float bufferOccupancyRatio = 0.47f;
    float emergencyOccupancyRatio = 0.66f;
    float reflectionOccupancyRatio = 0.32f;
    float retainTailRatio = 0.18f;
    std::uint32_t minimumRetainedTailTokens = 4096;
    std::uint32_t minimumChunkTokens = 8192;
    bool emitEventTurns = true;
    RollingModelConfig observer;
    RollingModelConfig reflector;
    RollingModelConfig workingMemoryUpdater;

    bool operator==(const RollingMemoryConfig &other) const = default;
  };

  std::string providerId = "nanogpt"; ///< LLM provider identifier.
  std::string modelId;                ///< LLM model identifier.
  std::string modelVariant;         ///< Selected model variant (if applicable).
  std::string personaName = "aster"; ///< Persona to load from prompts/.
  int maxTurns = 200;       ///< Maximum autonomous turns before stopping.
  float temperature = 0.7f; ///< LLM generation temperature.
  std::optional<std::uint32_t> maxTokens; ///< Optional max output tokens.
  std::vector<std::string> stop;          ///< Optional stop sequences.
  bool persistHistory =
      true; ///< If false, agent runs detached with no journal.
  int maxIdenticalToolCalls =
      5; ///< Max consecutive identical tool calls before intervention
  int maxInsanityRetries = 2;        ///< Max retry attempts when insanity is detected
  bool insanityDetectionEnabled = true;
  int insanityRepetitionThreshold = 3; ///< Min consecutive repeats to flag
  std::uint64_t insanityMaxTokenThreshold = 50000; ///< Max tokens before flagging
  RollingMemoryConfig rollingMemory;

  bool operator==(const AgentConfig &other) const = default;
};

/**
 * @brief A single turn in the conversation (Assistant response + following
 * actions).
 */
struct AgentTurn {
  std::string turnId;            ///< Unique turn ID.
  std::vector<Message> messages; ///< Messages associated with this turn.
  AgentMetrics metrics;          ///< Performance metrics for this turn.
  StopReason stopReason =
      StopReason::Stop; ///< Why this turn's generation ended.

  bool operator==(const AgentTurn &other) const = default;
};

/**
 * @brief Full conversation history (chronological turns).
 */
struct AgentHistory {
  std::string threadId;         ///< Unique thread/session ID.
  std::vector<AgentTurn> turns; ///< History of all turns.

  bool operator==(const AgentHistory &other) const = default;
};

/**
 * @brief The "Entire Universe" source of truth for an agent instance.
 */
struct AgentContext {
  AgentIdentity identity;       ///< Who the agent is.
  AgentPermissions permissions; ///< What the agent can do.
  HostEnvironment environment;  ///< Where the agent lives.
  std::shared_ptr<AgentHistory> history =
      std::make_shared<AgentHistory>(); ///< What the agent has done.
  AgentState state;                     ///< What the agent is doing now.
  AgentMetrics aggregateMetrics;        ///< Total performance telemetry.
  AgentConfig config; ///< Generation and lifecycle configuration.

  bool operator==(const AgentContext &other) const {
    if (history && other.history) {
      if (!(*history == *other.history))
        return false;
    } else if (history || other.history) {
      return false;
    }
    return identity == other.identity && permissions == other.permissions &&
           environment == other.environment && state == other.state &&
           aggregateMetrics == other.aggregateMetrics && config == other.config;
  }
};

/**
 * @brief Metadata for a collaborative thread.
 */
struct ThreadMetadata {
  struct RetryableRequest {
    std::string targetAgentId;
    std::string turnId;
    std::string text;
    std::vector<ImageContent> images;
    uint64_t recordedAt = 0;
    bool eligible = false; ///< Deprecated compatibility field.

    bool operator==(const RetryableRequest &other) const = default;
  };

  std::string threadId;
  std::string title;
  HostCreationOptions hostOptions;
  std::string hostIdentifier;
  std::string cwd;
  std::string leadPersona;
  /// Optional initial mode the user picked on the welcome screen. Empty
  /// means "no mode active at start"; otherwise the lead agent boots into
  /// this mode (e.g. "diagnose", "forge:prime"). Persisted with the thread
  /// metadata so resuming reproduces the original stance.
  std::string initialMode;
  bool isBenchmarkRun = false;
  std::string benchmarkId;
  std::string benchmarkTaskId;
  std::string activePlanId;
  std::optional<RetryableRequest> lastRetryableRequest;
  ThreadPermissionMode permissionMode = ThreadPermissionMode::Request;
  uint64_t createdAt = 0;
  uint64_t lastActiveAt = 0;

  bool operator==(const ThreadMetadata &other) const = default;
};

/**
 * @brief Metadata for a thread-scoped artifact file.
 */
struct ThreadArtifactMetadata {
  std::string threadId;
  std::string ownerAgentId;
  std::string ownerFriendlyName;
  std::string filename;
  std::string storagePath;
  uint64_t createdAt = 0;
  uint64_t updatedAt = 0;
  std::optional<std::string> kind;
  std::optional<std::string> description;

  bool operator==(const ThreadArtifactMetadata &other) const = default;
};

/**
 * @brief Persisted edit history records for exact file-undo support.
 */

enum class EditBatchStatus {
  Applied,
  Undone,
  Redone,
};

enum class EditFileMutationStatus {
  Applied,
  Redone,
  BlockedByLaterEdits,
  Diverged,
  Undone,
};

enum class EditUndoResultStatus {
  Succeeded,
  RejectedAlreadyUndone,
  RejectedBlocked,
  RejectedDiverged,
  RejectedBatchNotFullyUndoable,
  RejectedPartialFailure,
};

struct EditMutationOperation {
  std::string description;
  int startLine = 1;
  int endLine = 0;
  std::vector<std::string> oldLines;
  std::vector<std::string> newLines;

  bool operator==(const EditMutationOperation &other) const = default;
};

struct EditFileMutation {
  std::string fileMutationId;
  std::string editBatchId;
  std::string threadId;
  std::string filePath;
  int ordinalInBatch = 0;
  bool hadFileBefore = false;
  bool hasFileAfter = false;
  std::string preHash;
  std::string postHash;
  std::uint64_t preSize = 0;
  std::uint64_t postSize = 0;
  std::string newlineModeBefore;
  std::string newlineModeAfter;
  EditFileMutationStatus status = EditFileMutationStatus::Applied;
  std::string mode;
  std::vector<EditMutationOperation> operations;
  std::string diffPreview;

  bool operator==(const EditFileMutation &other) const = default;
};

struct EditBatchSummary {
  std::string editBatchId;
  std::string threadId;
  std::string agentId;
  std::string parentAgentId;
  std::string friendlyName;
  std::string turnId;
  std::string toolCallId;
  std::string toolName;
  std::string requestMode;
  std::uint64_t createdAt = 0;
  EditBatchStatus status = EditBatchStatus::Applied;
  std::vector<std::string> files;
  int addedLines = 0;
  int removedLines = 0;
  std::string summaryText;
  std::optional<std::string> undoActionBatchId;

  bool operator==(const EditBatchSummary &other) const = default;
};

struct EditBatchDetail {
  EditBatchSummary summary;
  std::vector<EditFileMutation> files;

  bool operator==(const EditBatchDetail &other) const = default;
};

struct EditUndoEligibility {
  std::string editBatchId;
  bool undoable = false;
  EditUndoResultStatus resultStatus = EditUndoResultStatus::Succeeded;
  std::vector<std::string> blockingEditBatchIds;
  std::vector<std::string> divergedFiles;
  std::string reason;

  bool operator==(const EditUndoEligibility &other) const = default;
};

struct EditUndoAction {
  std::string undoActionId;
  std::string threadId;
  std::string requestedByAgentId;
  std::string targetEditBatchId;
  std::uint64_t createdAt = 0;
  EditUndoResultStatus resultStatus = EditUndoResultStatus::Succeeded;
  std::string resultJson;

  bool operator==(const EditUndoAction &other) const = default;
};

struct EditRedoEligibility {
  std::string undoActionId;
  bool redoable = false;
  std::vector<std::string> blockingEditBatchIds;
  std::vector<std::string> divergedFiles;
  std::string reason;

  bool operator==(const EditRedoEligibility &other) const = default;
};

struct EditRedoAction {
  std::string redoActionId;
  std::string threadId;
  std::string targetUndoActionId;
  std::uint64_t createdAt = 0;
  std::string resultJson;
  bool operator==(const EditRedoAction &other) const = default;
};

struct EditHistoryFilters {
  std::optional<std::string> agentId;
  std::optional<std::string> parentAgentId;
  bool includeUndone = true;

  bool operator==(const EditHistoryFilters &other) const = default;
};

struct TranscriptUndoAction {
  std::string undoActionId;
  std::string threadId;
  std::string agentId;
  std::string scopeType;
  std::string scopeArgJson;
  // Compound undo support: edit undo actions performed as part of this transcript undo.
  std::vector<std::string> editUndoActionIds;
  std::uint64_t createdAt = 0;
  bool redoAvailable = false;
  std::string reason;

  bool operator==(const TranscriptUndoAction &other) const = default;
};

struct TranscriptRedoPayload {
  std::string undoActionId;
  std::string threadId;
  std::string agentId;
  int ordinal = 0;
  std::vector<AgentTurn> turns;
  bool operator==(const TranscriptRedoPayload &other) const = default;
};

struct TranscriptRedoEligibility {
  std::string undoActionId;
  bool redoable = false;
  std::string reason;

  bool operator==(const TranscriptRedoEligibility &other) const = default;
};

struct TranscriptRedoAction {
  std::string redoActionId;
  std::string undoActionId;
  std::string threadId;
  std::string agentId;
  std::uint64_t createdAt = 0;
  std::string resultJson;

  bool operator==(const TranscriptRedoAction &other) const = default;
};


/**
 * @brief Execution task embedded beneath a chunk (V2 work language).
 * One level of task depth only; no nested subtasks.
 */
struct WorkTask {
  std::string id;    ///< Stable task identifier within chunk.
  std::string title; ///< Short task title.
  std::string goal;  ///< Task description / objective.
  WorkChunkStatus status = WorkChunkStatus::Ready; ///< Task execution status.
  std::string notes; ///< Optional constraints or implementation notes.
  std::string verificationCondition; ///< Plain-English verification criterion.
  std::string assignedWorkerId;      ///< Optional worker agent ID if delegated.
  uint64_t createdAt = 0;
  uint64_t updatedAt = 0;

  bool operator==(const WorkTask &other) const = default;
};

/**
 * @brief Smallest persisted unit of plan execution (V2 work language).
 * Chunks may be flat (no tasks) or task-bearing (one level of tasks).
 */
struct WorkChunk {
  std::string id;
  std::string title;
  std::string goal;
  std::string context;
  std::string constraints;
  std::string completion;
  bool planningGate = false;
  WorkChunkStatus status = WorkChunkStatus::Ready;
  std::vector<std::string> dependsOn;
  std::string assignedAgentId;
  int attemptCount = 0;
  std::string resultSummary;
  std::string reviewSummary;
  uint64_t createdAt = 0;
  uint64_t updatedAt = 0;

  // V2 richer chunk spec fields
  std::vector<std::string> filesToRead; ///< Files the executor should read.
  std::vector<std::string>
      filesToTouch;                  ///< Files expected to be modified/created.
  std::string cwd;                   ///< Working directory for execution.
  std::string verificationCondition; ///< Plain-English acceptance criterion.
  std::string handoffNotes;          ///< Context for executor handoff.

  // V2 task structure (one level deep only)
  std::vector<WorkTask> tasks;

  bool operator==(const WorkChunk &other) const = default;
};

/**
 * @brief Persisted thread-owned plan with embedded chunks.
 */
struct Plan {
  std::string id;
  std::string threadId;
  std::string title;
  std::string objective;
  std::string context;
  std::string strategy;
  PlanStatus status = PlanStatus::Draft;
  std::string notes;
  uint64_t createdAt = 0;
  uint64_t updatedAt = 0;
  std::vector<WorkChunk> chunks;

  bool operator==(const Plan &other) const = default;
};

/**
 * @brief Persisted per-agent todo item used as personal execution state.
 */
struct TodoItem {
  int id = 0;
  std::string text;
  TodoStatus status = TodoStatus::Pending;
  std::string chunkId;
  std::string planId;
  uint64_t createdAt = 0;
  uint64_t updatedAt = 0;

  bool operator==(const TodoItem &other) const = default;
};

/**
 * @brief Persisted todo list for one specific agent in one thread.
 */
struct AgentTodoList {
  std::string threadId;
  std::string agentId;
  int nextId = 1;
  std::vector<TodoItem> items;

  bool operator==(const AgentTodoList &other) const = default;
};

} // namespace firmius::shared

#endif

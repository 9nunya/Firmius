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
  std::string friendlyName; ///< Machine-friendly slug name (e.g., "lead",
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
  std::string personaName = "lead"; ///< Persona to load from prompts/.
  int maxTurns = 200;       ///< Maximum autonomous turns before stopping.
  float temperature = 0.7f; ///< LLM generation temperature.
  std::optional<std::uint32_t> maxTokens; ///< Optional max output tokens.
  std::vector<std::string> stop;          ///< Optional stop sequences.
  bool persistHistory =
      true; ///< If false, agent runs detached with no journal.
  int maxIdenticalToolCalls =
      5; ///< Max consecutive identical tool calls before intervention
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

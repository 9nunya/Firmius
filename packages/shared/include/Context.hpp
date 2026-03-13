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
      ownedProcesses;                 ///< IDs of spawned background processes.
  std::vector<std::string> readFiles; ///< Paths of files read in this session.
  std::vector<std::string> fullyReadFiles; ///< Set of paths that were read entirely
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
  std::string providerId = "nanogpt"; ///< LLM provider identifier.
  std::string modelId;                ///< LLM model identifier.
  std::string modelVariant; ///< Selected model variant (if applicable).
  std::string personaName = "coder"; ///< Persona to load from prompts/.
  int maxTurns = 200;       ///< Maximum autonomous turns before stopping.
  float temperature = 0.7f; ///< LLM generation temperature.
  std::optional<std::uint32_t> maxTokens; ///< Optional max output tokens.
  std::vector<std::string> stop;          ///< Optional stop sequences.
  bool persistHistory =
      true; ///< If false, agent runs detached with no journal.
  int maxIdenticalToolCalls =
      5; ///< Max consecutive identical tool calls before intervention

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
  std::string threadId;
  std::string title;
  HostCreationOptions hostOptions;
  std::string hostIdentifier;
  std::string cwd;
  std::string leadPersona;
  uint64_t createdAt = 0;
  uint64_t lastActiveAt = 0;

  bool operator==(const ThreadMetadata &other) const = default;
};

} // namespace firmius::shared

#endif

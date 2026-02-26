#ifndef FIRMIUS_SHARED_CONTEXT_HPP
#define FIRMIUS_SHARED_CONTEXT_HPP

#include "Enums.hpp"
#include "Message.hpp"
#include "Metrics.hpp"

#include <string>
#include <vector>
#include <map>
#include <optional>

/**
 * @brief State and environment context for the agent engine.
 */
namespace firmius::shared {

/**
 * @brief Identity and personality metadata for an agent.
 */
struct AgentIdentity {
  std::string id;            ///< Unique identity ID.
  std::string name;          ///< Display name of the agent.
  std::string role;          ///< Assigned role (e.g., "coder").
  std::string goal;          ///< High-level mission goal.
  std::string systemPrompt;  ///< Raw persona instructions.

  bool operator==(const AgentIdentity& other) const = default;
};

/**
 * @brief Sandbox permission set for an agent.
 */
struct AgentPermissions {
  std::vector<ToolScope> allowedScopes; ///< Tools the agent is allowed to invoke.
  std::vector<std::string> allowedPaths; ///< Filesystem paths the agent can access.
  bool allowOutsideCwd = false;          ///< If true, allows access beyond /work.

  bool operator==(const AgentPermissions& other) const = default;
};

/**
 * @brief Host environment details.
 */
struct HostEnvironment {
  HostType type;            ///< Environment type (Local/Docker).
  std::string identifier;   ///< Host identifier (e.g., container ID).
  std::string cwd;          ///< Current working directory.
  std::map<std::string, std::string> envVars; ///< Environment variables.

  bool operator==(const HostEnvironment& other) const = default;
};

/**
 * @brief Real-time runtime state of the agent.
 */
struct AgentState {
  AgentStatus currentStatus = AgentStatus::Idle; ///< Current lifecycle status.
  std::vector<std::string> pendingToolCalls;    ///< IDs of tools currently executing.
  std::vector<std::string> ownedProcesses;      ///< IDs of spawned background processes.
  std::optional<std::string> fatalError;         ///< Description of fatal failure, if any.

  bool operator==(const AgentState& other) const = default;
};

/**
 * @brief A single turn in the conversation (Assistant response + following actions).
 */
struct AgentTurn {
  std::string turnId;            ///< Unique turn ID.
  std::vector<Message> messages; ///< Messages associated with this turn.
  AgentMetrics metrics;          ///< Performance metrics for this turn.

  bool operator==(const AgentTurn& other) const = default;
};

/**
 * @brief Full conversation history (chronological turns).
 */
struct AgentHistory {
  std::string threadId;        ///< Unique thread/session ID.
  std::vector<AgentTurn> turns; ///< History of all turns.

  bool operator==(const AgentHistory& other) const = default;
};

/**
 * @brief The "Entire Universe" source of truth for an agent instance.
 */
struct AgentContext {
  AgentIdentity identity;           ///< Who the agent is.
  AgentPermissions permissions;     ///< What the agent can do.
  HostEnvironment environment;      ///< Where the agent lives.
  AgentHistory history;             ///< What the agent has done.
  AgentState state;                 ///< What the agent is doing now.
  AgentMetrics aggregateMetrics;    ///< Total performance telemetry.

  bool operator==(const AgentContext& other) const = default;
};

}

#endif

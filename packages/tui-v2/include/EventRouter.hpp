#pragma once

#include "AppState.hpp"
#include "Enums.hpp"
#include "daemon/Protocol.hpp"

#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace firmius::tui2 {

/// Parses DaemonEventEnvelopes and dispatches state mutations to AppState.
class EventRouter {
public:
  explicit EventRouter(AppState &state);

  /// Route a daemon event envelope to appropriate state mutations.
  void route(const firmius::daemon::DaemonEventEnvelope &envelope);

  /// Route a runtime event by type string + JSON payload.
  void routeRuntimeEvent(const std::string &eventType, const std::string &eventJson,
                         const std::string &threadId, const std::string &agentId,
                         std::optional<firmius::shared::AgentStatus> realStatus = std::nullopt);

private:
  void handleAgentText(const std::string &json, const std::string &agentId);
  void handleAgentThinking(const std::string &json, const std::string &agentId);
  void handleAgentToolCall(const std::string &json, const std::string &agentId);
  void handleAgentToolCallChunk(const std::string &json, const std::string &agentId);
  void handleAgentFileEdited(const std::string &json);
  void handleAgentProcessSpawned(const std::string &json, const std::string &agentId);
  void handleAgentProcessOutput(const std::string &json, const std::string &agentId);
  void handleAgentTurnCompleted(const std::string &json, const std::string &agentId);
  void handleAgentFinished(const std::string &agentId);
  void handleAgentSpawned(const std::string &json, const std::string &agentId);
  void handleAgentError(const std::string &json, const std::string &agentId);
  void handleAgentInterrupted(const std::string &agentId);
  void handleUserMessageSent(const std::string &json);
  void handleMessageQueued();
  void handleMessageDequeued();
  void handlePermissionEscalation(const std::string &json);
  void handlePermissionResolved(const std::string &json);
  void handleAgentTodoUpdated(const std::string &json);
  void handleModelSwitched(const std::string &json);
  void handleThreadTitleUpdated(const std::string &json);
  void handleConfigUpdated();

  AppState &state_;

  // Buffered process output for race condition: AgentProcessOutput may arrive
  // before AgentProcessSpawned creates the processId→toolCallId mapping.
  struct PendingProcessOutput {
    std::string output;
    bool isStderr;
    bool finished;
    int exitCode;
    double durationMs;
  };
  std::unordered_map<std::string, std::vector<PendingProcessOutput>> pendingProcessOutput_;

  void flushPendingProcessOutput(const std::string &processId);
};

} // namespace firmius::tui2

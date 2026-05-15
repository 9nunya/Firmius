#pragma once

#include "AppState.hpp"
#include "daemon/Protocol.hpp"

#include <string>

namespace firmius::tui2 {

/// Parses DaemonEventEnvelopes and dispatches state mutations to AppState.
class EventRouter {
public:
  explicit EventRouter(AppState &state);

  /// Route a daemon event envelope to appropriate state mutations.
  void route(const firmius::daemon::DaemonEventEnvelope &envelope);

  /// Route a runtime event by type string + JSON payload.
  void routeRuntimeEvent(const std::string &eventType,
                         const std::string &eventJson,
                         const std::string &threadId,
                         const std::string &agentId);

private:
  void handleAgentText(const std::string &json, const std::string &agentId);
  void handleAgentThinking(const std::string &json, const std::string &agentId);
  void handleAgentToolCall(const std::string &json, const std::string &agentId);
  void handleAgentTurnCompleted(const std::string &agentId);
  void handleAgentFinished(const std::string &agentId);
  void handleAgentSpawned(const std::string &json, const std::string &agentId);
  void handleAgentError(const std::string &json, const std::string &agentId);
  void handleUserMessageSent(const std::string &json);
  void handleMessageQueued();
  void handleMessageDequeued();
  void handlePermissionEscalation(const std::string &json);
  void handlePermissionResolved(const std::string &json);
  void handleAgentProcessOutput(const std::string &json, const std::string &agentId);

  AppState &state_;
};

} // namespace firmius::tui2

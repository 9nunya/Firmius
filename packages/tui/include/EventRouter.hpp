#ifndef FIRMIUS_TUI_EVENTROUTER_HPP
#define FIRMIUS_TUI_EVENTROUTER_HPP

#include "AppState.hpp"
#include "Enums.hpp"
#include "daemon/Protocol.hpp"

#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace firmius::tui {

/// Parses DaemonEventEnvelopes and dispatches state mutations to AppState.
class EventRouter {
public:
  using ConnectProgressCallback =
      std::function<void(const firmius::daemon::ConnectProgressSnapshot &)>;
  using RewindAppliedCallback =
      std::function<void(const firmius::daemon::RewindAppliedSnapshot &)>;

  explicit EventRouter(AppState &state);

  /// Route a daemon event envelope to appropriate state mutations.
  void route(const firmius::daemon::DaemonEventEnvelope &envelope);

  /// Route a runtime event by type string + JSON payload.
  void routeRuntimeEvent(const std::string &eventType, const std::string &eventJson,
                         const std::string &threadId, const std::string &agentId,
                         std::optional<firmius::shared::AgentStatus> realStatus = std::nullopt);

  /// Subscribe to ConnectProgress events. Used by App to forward into the
  /// active ConnectOverlay. EventRouter doesn't own the overlay, so it just
  /// hands the snapshot off to whoever wants it.
  void setOnConnectProgress(ConnectProgressCallback cb) {
    onConnectProgress_ = std::move(cb);
  }

  /// Subscribe to RewindApplied events. Used by App to refresh transcript
  /// + close the rewind overlay after a compound rewind succeeds.
  void setOnRewindApplied(RewindAppliedCallback cb) {
    onRewindApplied_ = std::move(cb);
  }

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
  void handleMessageQueued(const std::string &json);
  void handleMessageDequeued(const std::string &json);
  void handlePermissionEscalation(const std::string &json);
  void handlePermissionResolved(const std::string &json);
  void handleAgentTodoUpdated(const std::string &json);
  void handleModelSwitched(const std::string &json);
  void handleThreadTitleUpdated(const std::string &json);
  void handleThreadMetadataUpdated(const std::string &json);
  void handleConfigUpdated();
  void handleHookStateChanged(const firmius::daemon::HookStateSnapshot &snapshot);
  void handleEmbeddingModelProgress(const std::string &json);
  void handleAgentCompacting(const std::string &agentId);
  void handleContextCompacted(const std::string &json, const std::string &agentId);

  AppState &state_;
  ConnectProgressCallback onConnectProgress_;
  RewindAppliedCallback onRewindApplied_;

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

} // namespace firmius::tui

#endif // FIRMIUS_TUI_EVENTROUTER_HPP

#pragma once

#include "daemon/DaemonClient.hpp"
#include "daemon/Protocol.hpp"

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace firmius::tui2 {

/// Manages the daemon connection lifecycle.
/// Auto-detects running daemon or auto-starts firmiusd.
class DaemonSession {
public:
  using EventListener = std::function<void(const firmius::daemon::DaemonEventEnvelope &)>;

  DaemonSession();
  ~DaemonSession();

  /// Connect to daemon (auto-start if needed). Returns true on success.
  bool connect();

  /// Disconnect from daemon.
  void disconnect();

  /// Whether we have an active connection.
  bool connected() const;

  /// Subscribe to daemon events with the given listener.
  bool subscribe(EventListener listener);

  /// Get the client session snapshot.
  firmius::daemon::ClientSessionSnapshot session() const;

  // ── Thread operations ──
  std::vector<firmius::shared::ThreadMetadata> listThreads() const;
  std::vector<firmius::daemon::ThreadOverview>
  listThreadOverviews(const std::string &cwd) const;
  std::optional<firmius::daemon::ThreadSnapshot> getThread(
      const std::string &threadId) const;
  firmius::daemon::ThreadsCreateResponse createThread(const std::string &cwd,
                                                       const std::string &persona,
                                                       const std::string &mode);
  firmius::daemon::ThreadsOpenResponse openThread(const std::string &threadId);
  firmius::daemon::ThreadsSendResponse send(const std::string &threadId,
                                             const std::string &agentId,
                                             const std::string &text,
                                             std::vector<firmius::shared::ImageContent> images = {});

  // ── Transcript ──
  std::optional<firmius::daemon::TranscriptSnapshot> getTranscript(
      const std::string &threadId, const std::string &agentId) const;

  // ── Tool calls ──
  std::vector<firmius::daemon::ToolCallSnapshot> listToolCalls(
      const std::string &threadId, const std::string &agentId) const;

  // ── Agent operations ──
  std::optional<firmius::daemon::AgentRuntimeSnapshot> getAgent(
      const std::string &threadId, const std::string &agentId) const;
  std::optional<firmius::daemon::AgentTodoSnapshot> getAgentTodo(
      const std::string &threadId, const std::string &agentId) const;
  firmius::daemon::AgentTreeSnapshot listAgents(
      const std::string &threadId) const;
  std::optional<firmius::daemon::AgentRuntimeSnapshot> focusAgent(
      const std::string &threadId, const std::string &agentId) const;
  std::optional<firmius::daemon::AgentRuntimeSnapshot> interruptAgent(
      const std::string &threadId, const std::string &agentId);
  std::optional<firmius::daemon::AgentRuntimeSnapshot>
  abortAndFlushQueuedMessages(const std::string &threadId,
                              const std::string &agentId);
  std::optional<firmius::daemon::AgentRuntimeSnapshot> switchModel(
      const std::string &agentId,
      const std::string &providerId,
      const std::string &modelId,
      const std::string &variantName = "");

  // ── Permissions ──
  bool resolvePermission(const std::string &requestId,
                         firmius::shared::PermissionResponse response);
  bool resolvePermissionWithRules(
      const std::string &requestId,
      const std::vector<std::string> &selectedSuggestionIds);
  firmius::daemon::PermissionListRulesResponse listPolicyRules();
  firmius::daemon::PermissionUpsertRuleResponse upsertPolicyRule(
      const firmius::daemon::PolicyRuleWire &rule);
  firmius::daemon::PermissionDeleteRuleResponse deletePolicyRule(
      const std::string &ruleId);
  firmius::daemon::PermissionReloadPolicyResponse reloadPolicy();

  // ── Workflows ──
  bool executeWorkflow(const std::string &workflowId,
                       const std::vector<std::string> &args = {});

  // ── /connect wizard ──
  firmius::daemon::ConnectBeginResponse beginConnect(const std::string &providerId,
                                                       bool addAdditional);
  firmius::daemon::ConnectSubmitResponse submitConnect(const std::string &sessionId,
                                                         const std::string &answer);
  firmius::daemon::ConnectFinalizeResponse finalizeConnect(const std::string &sessionId);
  firmius::daemon::ConnectCancelResponse cancelConnect(const std::string &sessionId);

  // ── /undo Rewind ──
  firmius::daemon::RewindPreviewResponse
  previewRewind(const std::string &threadId, const std::string &agentId,
                const std::string &targetTurnId);
  firmius::daemon::RewindExecuteResponse
  executeRewind(const std::string &threadId, const std::string &agentId,
                const std::string &targetTurnId,
                firmius::daemon::RewindMode mode);

  // ── /redo ──
  firmius::daemon::RedoPreviewResponse
  previewRedo(const std::string &threadId, const std::string &agentId,
              int limit);
  firmius::daemon::RedoExecuteResponse
  executeRedo(const std::string &threadId, const std::string &agentId,
              const std::string &undoActionId,
              firmius::daemon::RedoMode mode);

  /// Direct access to the underlying DaemonClient for RPC calls not
  /// wrapped by convenience methods above.
  firmius::daemon::DaemonClient& client() { return *client_; }
  const firmius::daemon::DaemonClient& client() const { return *client_; }

private:
  std::unique_ptr<firmius::daemon::DaemonClient> client_;
};

} // namespace firmius::tui2

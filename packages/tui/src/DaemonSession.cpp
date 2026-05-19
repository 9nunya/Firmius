#include "DaemonSession.hpp"

#include <cstdlib>
#include <filesystem>

#if defined(_WIN32)
#include <process.h>
#define GET_PID() _getpid()
#else
#include <unistd.h>
#define GET_PID() getpid()
#endif

namespace firmius::tui {

DaemonSession::DaemonSession() = default;
DaemonSession::~DaemonSession() { disconnect(); }

bool DaemonSession::connect() {
  firmius::daemon::DaemonClientOptions options;
  options.identity.clientId = "tui-" +
      std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
  options.identity.uiKind = "tui";
  options.identity.pid = static_cast<int>(GET_PID());
  options.identity.capabilityFlags = {"rpc", "events"};

  auto cwd = std::filesystem::current_path();
  options.presence.cwd = cwd.string();
  options.presence.workspaceRoot = cwd.string();
  options.presence.repoRoot = cwd.string();

  options.autoStart = true;
  options.subscribeToEvents = true;

  if (const char* endpoint = std::getenv("FIRMIUS_DAEMON_ENDPOINT")) {
    options.connection.endpoint = endpoint;
  }

  // Make sure the implicitly-spawned daemon's stdio doesn't shred our raw
  // alt-screen TUI. Default to ~/.firmius/daemon.log so users have somewhere
  // to look at startup phase messages and warnings; fall back to /dev/null.
  if (options.spawnedDaemonLogFile.empty()) {
    if (const char* home = std::getenv("HOME")) {
      const auto firmiusHome = std::filesystem::path(home) / ".firmius";
      std::error_code ec;
      std::filesystem::create_directories(firmiusHome, ec);
      if (!ec) {
        options.spawnedDaemonLogFile = (firmiusHome / "daemon.log").string();
      }
    }
  }

  client_ = std::make_unique<firmius::daemon::DaemonClient>(std::move(options));
  return client_->connect();
}

void DaemonSession::disconnect() {
  if (client_) {
    client_->disconnect();
    client_.reset();
  }
}

bool DaemonSession::connected() const {
  return client_ && client_->connected();
}

bool DaemonSession::subscribe(EventListener listener) {
  if (!client_) return false;
  return client_->subscribe(std::move(listener));
}

firmius::daemon::ClientSessionSnapshot DaemonSession::session() const {
  if (!client_) return {};
  return client_->session();
}

std::vector<firmius::shared::ThreadMetadata> DaemonSession::listThreads() const {
  if (!client_) return {};
  return client_->listThreads();
}

std::vector<firmius::daemon::ThreadOverview>
DaemonSession::listThreadOverviews(const std::string &cwd) const {
  if (!client_) return {};
  firmius::daemon::ThreadOverviewRequest request;
  request.cwd = cwd;
  return client_->listThreadOverviews(request);
}

std::optional<firmius::daemon::ThreadSnapshot> DaemonSession::getThread(
    const std::string &threadId) const {
  if (!client_ || threadId.empty()) return std::nullopt;
  firmius::daemon::ThreadsOpenRequest request;
  request.threadId = threadId;
  return client_->getThread(request);
}

firmius::daemon::ThreadsCreateResponse DaemonSession::createThread(
    const std::string &cwd, const std::string &persona, const std::string &mode) {
  if (!client_) return {};
  firmius::daemon::ThreadsCreateRequest request;
  request.cwd = cwd.empty() ? std::filesystem::current_path().string() : cwd;
  request.leadPersona = persona;
  request.initialMode = mode;
  return client_->createThread(request);
}

firmius::daemon::ThreadsOpenResponse DaemonSession::openThread(
    const std::string &threadId) {
  if (!client_) return {};
  return client_->openThread(threadId);
}

firmius::daemon::ThreadsSendResponse DaemonSession::send(
    const std::string &threadId, const std::string &agentId,
    const std::string &text,
    std::vector<firmius::shared::ImageContent> images) {
  if (!client_) return {};
  firmius::daemon::ThreadsSendRequest request;
  request.threadId = threadId;
  request.agentId = agentId;
  request.text = text;
  request.images = std::move(images);
  return client_->send(request);
}

std::optional<firmius::daemon::TranscriptSnapshot> DaemonSession::getTranscript(
    const std::string &threadId, const std::string &agentId) const {
  if (!client_) return std::nullopt;
  firmius::daemon::TranscriptGetRequest request;
  request.threadId = threadId;
  request.agentId = agentId;
  return client_->getTranscript(request);
}

std::vector<firmius::daemon::ToolCallSnapshot> DaemonSession::listToolCalls(
    const std::string &threadId, const std::string &agentId) const {
  if (!client_) return {};
  firmius::daemon::ToolCallsListRequest request;
  request.threadId = threadId;
  request.agentId = agentId;
  return client_->listToolCalls(request);
}

std::optional<firmius::daemon::AgentRuntimeSnapshot> DaemonSession::getAgent(
    const std::string &threadId, const std::string &agentId) const {
  if (!client_) return std::nullopt;
  firmius::daemon::AgentTargetRequest request;
  request.threadId = threadId;
  request.agentId = agentId;
  return client_->getAgent(request);
}

std::optional<firmius::daemon::AgentTodoSnapshot> DaemonSession::getAgentTodo(
    const std::string &threadId, const std::string &agentId) const {
  if (!client_) return std::nullopt;
  firmius::daemon::AgentTargetRequest request;
  request.threadId = threadId;
  request.agentId = agentId;
  return client_->getAgentTodo(request);
}

firmius::daemon::AgentTreeSnapshot DaemonSession::listAgents(
    const std::string &threadId) const {
  if (!client_) return {};
  firmius::daemon::AgentTargetRequest request;
  request.threadId = threadId;
  return client_->listAgents(request);
}

std::optional<firmius::daemon::AgentRuntimeSnapshot> DaemonSession::focusAgent(
    const std::string &threadId, const std::string &agentId) const {
  if (!client_) return std::nullopt;
  firmius::daemon::AgentTargetRequest request;
  request.threadId = threadId;
  request.agentId = agentId;
  return client_->focusAgent(request);
}

std::optional<firmius::daemon::AgentRuntimeSnapshot> DaemonSession::interruptAgent(
    const std::string &threadId, const std::string &agentId) {
  if (!client_) return std::nullopt;
  firmius::daemon::AgentTargetRequest request;
  request.threadId = threadId;
  request.agentId = agentId;
  if (!client_->interruptAgent(request)) return std::nullopt;
  return getAgent(threadId, agentId);
}

std::optional<firmius::daemon::AgentRuntimeSnapshot>
DaemonSession::abortAndFlushQueuedMessages(const std::string &threadId,
                                           const std::string &agentId) {
  if (!client_) return std::nullopt;
  firmius::daemon::AgentTargetRequest request;
  request.threadId = threadId;
  request.agentId = agentId;
  if (!client_->abortAndFlushQueuedMessages(request)) return std::nullopt;
  return getAgent(threadId, agentId);
}

std::optional<firmius::daemon::AgentRuntimeSnapshot> DaemonSession::switchModel(
    const std::string &agentId,
    const std::string &providerId,
    const std::string &modelId,
    const std::string &variantName) {
  if (!client_) return std::nullopt;
  firmius::daemon::ModelSwitchRequest request;
  request.agentId = agentId;
  request.providerId = providerId;
  request.modelId = modelId;
  request.variantName = variantName;
  return client_->switchModel(request);
}

bool DaemonSession::resolvePermission(const std::string &requestId,
                                       firmius::shared::PermissionResponse response) {
  if (!client_) return false;
  firmius::daemon::PermissionResolveRequest request;
  request.requestId = requestId;
  request.response = response;
  return client_->resolvePermission(request);
}

bool DaemonSession::resolvePermissionWithRules(
    const std::string &requestId,
    const std::vector<std::string> &selectedSuggestionIds) {
  if (!client_) return false;
  firmius::daemon::PermissionResolveWithRulesRequest req;
  req.requestId = requestId;
  req.selectedSuggestionIds = selectedSuggestionIds;
  return client_->resolvePermissionWithRules(req);
}

firmius::daemon::PermissionListRulesResponse DaemonSession::listPolicyRules() {
  if (!client_) return {};
  return client_->listPolicyRules();
}

firmius::daemon::PermissionUpsertRuleResponse
DaemonSession::upsertPolicyRule(const firmius::daemon::PolicyRuleWire &rule) {
  if (!client_) return {};
  firmius::daemon::PermissionUpsertRuleRequest req;
  req.rule = rule;
  return client_->upsertPolicyRule(req);
}

firmius::daemon::PermissionDeleteRuleResponse
DaemonSession::deletePolicyRule(const std::string &ruleId) {
  if (!client_) return {};
  firmius::daemon::PermissionDeleteRuleRequest req;
  req.ruleId = ruleId;
  return client_->deletePolicyRule(req);
}

firmius::daemon::PermissionReloadPolicyResponse DaemonSession::reloadPolicy() {
  if (!client_) return {};
  return client_->reloadPolicy();
}

bool DaemonSession::executeWorkflow(const std::string &workflowId,
                                     const std::vector<std::string> &args) {
  if (!client_) return false;
  firmius::daemon::WorkflowExecuteRequest request;
  request.workflowId = workflowId;
  request.args = args;
  return client_->executeWorkflow(request);
}

firmius::daemon::ConnectBeginResponse
DaemonSession::beginConnect(const std::string &providerId, bool addAdditional) {
  firmius::daemon::ConnectBeginResponse empty;
  if (!client_) {
    empty.errorMessage = "daemon not connected";
    return empty;
  }
  firmius::daemon::ConnectBeginRequest request;
  request.providerId = providerId;
  request.addAdditional = addAdditional;
  return client_->beginConnect(request);
}

firmius::daemon::ConnectSubmitResponse
DaemonSession::submitConnect(const std::string &sessionId,
                              const std::string &answer) {
  firmius::daemon::ConnectSubmitResponse empty;
  if (!client_) {
    empty.errorMessage = "daemon not connected";
    return empty;
  }
  firmius::daemon::ConnectSubmitRequest request;
  request.sessionId = sessionId;
  request.answer = answer;
  return client_->submitConnect(request);
}

firmius::daemon::ConnectFinalizeResponse
DaemonSession::finalizeConnect(const std::string &sessionId) {
  firmius::daemon::ConnectFinalizeResponse empty;
  if (!client_) {
    empty.errorMessage = "daemon not connected";
    return empty;
  }
  firmius::daemon::ConnectFinalizeRequest request;
  request.sessionId = sessionId;
  return client_->finalizeConnect(request);
}

firmius::daemon::ConnectCancelResponse
DaemonSession::cancelConnect(const std::string &sessionId) {
  firmius::daemon::ConnectCancelResponse empty;
  if (!client_) return empty;
  firmius::daemon::ConnectCancelRequest request;
  request.sessionId = sessionId;
  return client_->cancelConnect(request);
}

firmius::daemon::RewindPreviewResponse
DaemonSession::previewRewind(const std::string &threadId,
                              const std::string &agentId,
                              const std::string &targetTurnId) {
  firmius::daemon::RewindPreviewResponse empty;
  if (!client_) {
    empty.errorMessage = "daemon not connected";
    return empty;
  }
  firmius::daemon::RewindPreviewRequest request;
  request.threadId = threadId;
  request.agentId = agentId;
  request.targetTurnId = targetTurnId;
  return client_->previewRewind(request);
}

firmius::daemon::RewindExecuteResponse
DaemonSession::executeRewind(const std::string &threadId,
                              const std::string &agentId,
                              const std::string &targetTurnId,
                              firmius::daemon::RewindMode mode) {
  firmius::daemon::RewindExecuteResponse empty;
  if (!client_) {
    empty.errorMessage = "daemon not connected";
    return empty;
  }
  firmius::daemon::RewindExecuteRequest request;
  request.threadId = threadId;
  request.agentId = agentId;
  request.targetTurnId = targetTurnId;
  request.mode = mode;
  return client_->executeRewind(request);
}

firmius::daemon::RedoPreviewResponse
DaemonSession::previewRedo(const std::string &threadId,
                            const std::string &agentId,
                            int limit) {
  firmius::daemon::RedoPreviewResponse empty;
  if (!client_) {
    empty.errorMessage = "daemon not connected";
    return empty;
  }
  firmius::daemon::RedoPreviewRequest request;
  request.threadId = threadId;
  request.agentId = agentId;
  request.limit = limit;
  return client_->previewRedo(request);
}

firmius::daemon::RedoExecuteResponse
DaemonSession::executeRedo(const std::string &threadId,
                            const std::string &agentId,
                            const std::string &undoActionId,
                            firmius::daemon::RedoMode mode) {
  firmius::daemon::RedoExecuteResponse empty;
  if (!client_) {
    empty.errorMessage = "daemon not connected";
    return empty;
  }
  firmius::daemon::RedoExecuteRequest request;
  request.threadId = threadId;
  request.agentId = agentId;
  request.undoActionId = undoActionId;
  request.mode = mode;
  return client_->executeRedo(request);
}

} // namespace firmius::tui

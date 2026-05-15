#include "DaemonSession.hpp"

#include <cstdlib>
#include <filesystem>

namespace firmius::tui2 {

DaemonSession::DaemonSession() = default;
DaemonSession::~DaemonSession() { disconnect(); }

bool DaemonSession::connect() {
  firmius::daemon::DaemonClientOptions options;
  options.identity.clientId = "tui-v2-" +
      std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
  options.identity.uiKind = "tui-v2";
  options.identity.pid = static_cast<int>(getpid());
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

firmius::daemon::ThreadsCreateResponse DaemonSession::createThread(
    const std::string &cwd, const std::string &persona, const std::string &mode) {
  if (!client_) return {};
  firmius::daemon::ThreadsCreateRequest request;
  request.cwd = cwd.empty() ? std::filesystem::current_path().string() : cwd;
  request.leadPersona = persona;
  request.initialMode = mode;
  request.permissionMode = firmius::shared::ThreadPermissionMode::Request;
  return client_->createThread(request);
}

firmius::daemon::ThreadsOpenResponse DaemonSession::openThread(
    const std::string &threadId) {
  if (!client_) return {};
  return client_->openThread(threadId);
}

firmius::daemon::ThreadsSendResponse DaemonSession::send(
    const std::string &threadId, const std::string &agentId,
    const std::string &text) {
  if (!client_) return {};
  firmius::daemon::ThreadsSendRequest request;
  request.threadId = threadId;
  request.agentId = agentId;
  request.text = text;
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

std::optional<firmius::daemon::AgentRuntimeSnapshot> DaemonSession::interruptAgent(
    const std::string &threadId, const std::string &agentId) {
  if (!client_) return std::nullopt;
  firmius::daemon::AgentTargetRequest request;
  request.threadId = threadId;
  request.agentId = agentId;
  if (!client_->interruptAgent(request)) return std::nullopt;
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

} // namespace firmius::tui2

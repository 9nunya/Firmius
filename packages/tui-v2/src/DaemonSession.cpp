#include "DaemonSession.hpp"

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

std::optional<firmius::daemon::AgentRuntimeSnapshot> DaemonSession::interruptAgent(
    const std::string &threadId, const std::string &agentId) {
  (void)threadId;
  (void)agentId;
  // The DaemonClient doesn't expose interruptAgent directly —
  // this would need to be added to the client or use raw RPC.
  // For now, return nullopt.
  return std::nullopt;
}

bool DaemonSession::resolvePermission(const std::string & /*requestId*/,
                                       firmius::shared::PermissionResponse /*response*/) {
  // Permission resolve requires the DaemonClient to expose it.
  // The current client has setPermissionMode but not individual resolve.
  // Future: add resolvePermission to DaemonClient.
  return false;
}

} // namespace firmius::tui2

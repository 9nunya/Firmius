#include "daemon/DaemonServer.hpp"

#include "daemon/DaemonService.hpp"
#include "daemon/ProtocolSerialization.hpp"
#include "daemon/SocketTransport.hpp"

#include "lsp/JsonRpcTransport.hpp"

#include <rapidjson/document.h>
#include <atomic>
#include <chrono>
#include <memory>
#include <stdexcept>
#include <thread>

namespace firmius::daemon {

namespace {

rapidjson::Value emptyObject(rapidjson::Document::AllocatorType &allocator) {
  rapidjson::Value value(rapidjson::kObjectType);
  (void)allocator;
  return value;
}

const rapidjson::Value *requireObjectParams(
    const rapidjson::Document &request, const char *methodName,
    const rapidjson::Value &id, firmius::core::JsonRpcTransport &rpc) {
  if (!request.HasMember("params") || !request["params"].IsObject()) {
    rpc.sendError(id, -32602,
                  std::string(methodName) + " requires object params");
    return nullptr;
  }
  return &request["params"];
}

} // namespace

struct DaemonServer::ClientConnection {
  std::shared_ptr<firmius::core::JsonRpcTransport> rpc;
  std::jthread thread;
  std::atomic<bool> finished{false};
};

DaemonServer::DaemonServer(DaemonConnectionInfo info)
    : info_(std::move(info)), transport_(info_) {}

DaemonServer::~DaemonServer() { stop(); }

bool DaemonServer::start() {
  if (running_.exchange(true)) {
    return true;
  }
  service_ = std::make_unique<DaemonService>();
  service_->start();
  if (!transport_.listen()) {
    service_->shutdown();
    service_.reset();
    running_ = false;
    return false;
  }
  acceptThread_ = std::jthread([this](std::stop_token) { acceptLoop(); });
  return true;
}

void DaemonServer::stop() {
  if (!running_.exchange(false)) {
    return;
  }
  transport_.close();
  if (acceptThread_.joinable()) {
    acceptThread_.request_stop();
    acceptThread_.join();
  }
  stopClients();
  if (service_) {
    service_->shutdown();
    service_.reset();
  }
}

bool DaemonServer::running() const { return running_.load(); }

const DaemonConnectionInfo &DaemonServer::connectionInfo() const { return info_; }
const std::string &DaemonServer::lastError() const { return transport_.lastError(); }

void DaemonServer::acceptLoop() {
  while (running_.load()) {
    auto channel = transport_.accept();
    if (!channel.has_value()) {
      if (!running_.load()) {
        break;
      }
      reapFinishedClients();
      continue;
    }

    auto connection = std::make_shared<ClientConnection>();
    connection->rpc = std::make_shared<firmius::core::JsonRpcTransport>(
        std::move(channel->writer), std::move(channel->reader), std::move(channel->wakeStop));
    connection->thread = std::jthread(
        [this, connection](std::stop_token stopToken) {
          std::string registeredClientId;
          auto weakRpc = std::weak_ptr<firmius::core::JsonRpcTransport>(connection->rpc);
          connection->rpc->setRequestHandler(
              [this, &registeredClientId, weakRpc](const rapidjson::Document &request) {
                const auto &id = request["id"];
                const std::string method = request["method"].GetString();
                const rapidjson::Value *params =
                    request.HasMember("params") ? &request["params"] : nullptr;
                rapidjson::Document response;
                response.SetObject();
                auto &respAlloc = response.GetAllocator();

                auto rpc = weakRpc.lock();
                if (!rpc || !service_) {
                  return;
                }

                try {
                  if (method == kRpcUiSnapshotGet) {
                    params = requireObjectParams(request, kRpcUiSnapshotGet, id, *rpc);
                    if (!params) {
                      return;
                    }
                    auto snapshot = service_->uiSnapshot(
                        registeredClientId, uiSnapshotRequestFromJson(*params));
                    auto result = toJsonValue(snapshot, respAlloc);
                    rpc->sendResponse(id, result);
                    return;
                  }

                  if (method == kRpcDaemonPing) {
                    auto result = toJsonValue(service_->ping(), respAlloc);
                    rpc->sendResponse(id, result);
                    return;
                  }
                  if (method == kRpcDaemonAuditEmitRuntimeEvent) {
                    if (!params || !params->IsObject()) {
                      rpc->sendError(id, -32602, "invalid params");
                      return;
                    }
                    auto result = toJsonValue(
                        service_->auditEmitRuntimeEvent(
                            daemonAuditEmitRuntimeEventRequestFromJson(*params)),
                        respAlloc);
                    rpc->sendResponse(id, result);
                    return;
                  }
                  if (method == kRpcClientHello) {
                    params = requireObjectParams(request, kRpcClientHello, id, *rpc);
                    if (!params) {
                      return;
                    }
                    auto hello = clientHelloRequestFromJson(*params);
                    auto result = toJsonValue(service_->registerClient(hello), respAlloc);
                    registeredClientId = hello.identity.clientId;
                    rpc->sendResponse(id, result);
                    return;
                  }
                  if (method == kRpcClientGoodbye) {
                    service_->unsubscribe(registeredClientId);
                    service_->unregisterClient(registeredClientId);
                    auto result = emptyObject(respAlloc);
                    rpc->sendResponse(id, result);
                    return;
                  }
                  if (method == kRpcThreadsList) {
                    auto result = toJsonValue(service_->listThreads(), respAlloc);
                    rpc->sendResponse(id, result);
                    return;
                  }
                  if (method == kRpcThreadsGet) {
                    ThreadsOpenRequest get{
                        params && params->HasMember("thread_id")
                            ? (*params)["thread_id"].GetString()
                            : ""};
                    auto result =
                        toJsonValue(service_->getThread(registeredClientId, get), respAlloc);
                    rpc->sendResponse(id, result);
                    return;
                  }
                  if (method == kRpcThreadsOpen) {
                    ThreadsOpenRequest open{
                        params && params->HasMember("thread_id")
                            ? (*params)["thread_id"].GetString()
                            : ""};
                    auto result =
                        toJsonValue(service_->openThread(registeredClientId, open), respAlloc);
                    rpc->sendResponse(id, result);
                    return;
                  }
                  if (method == kRpcThreadsFocus) {
                    ThreadsOpenRequest focus{
                        params && params->HasMember("thread_id")
                            ? (*params)["thread_id"].GetString()
                            : ""};
                    auto result = toJsonValue(
                        service_->focusThread(registeredClientId, focus), respAlloc);
                    rpc->sendResponse(id, result);
                    return;
                  }
                  if (method == kRpcThreadsCreate) {
                    params = requireObjectParams(request, kRpcThreadsCreate, id, *rpc);
                    if (!params) {
                      return;
                    }
                    auto create = threadsCreateRequestFromJson(*params);
                    auto result =
                        toJsonValue(service_->createThread(registeredClientId, create), respAlloc);
                    rpc->sendResponse(id, result);
                    return;
                  }
                  if (method == kRpcThreadsSend) {
                    params = requireObjectParams(request, kRpcThreadsSend, id, *rpc);
                    if (!params) {
                      return;
                    }
                    auto send = threadsSendRequestFromJson(*params);
                    auto result =
                        toJsonValue(service_->sendToThread(registeredClientId, send), respAlloc);
                    rpc->sendResponse(id, result);
                    return;
                  }
                  if (method == kRpcAgentsList) {
                    auto target = params ? agentTargetRequestFromJson(*params)
                                         : AgentTargetRequest{};
                    auto result =
                        toJsonValue(service_->listAgents(registeredClientId, target),
                                    respAlloc);
                    rpc->sendResponse(id, result);
                    return;
                  }
                  if (method == kRpcAgentsGet) {
                    auto target = params ? agentTargetRequestFromJson(*params)
                                         : AgentTargetRequest{};
                    auto result = service_->getAgent(registeredClientId, target);
                    if (!result.has_value()) {
                      rpc->sendError(id, -32000, "agent not found");
                      return;
                    }
                    auto resultValue = toJsonValue(*result, respAlloc);
                    rpc->sendResponse(id, resultValue);
                    return;
                  }
                  if (method == kRpcAgentsFocus) {
                    params = requireObjectParams(request, kRpcAgentsFocus, id, *rpc);
                    if (!params) {
                      return;
                    }
                    auto target = agentTargetRequestFromJson(*params);
                    auto result = service_->focusAgent(registeredClientId, target);
                    if (!result.has_value()) {
                      rpc->sendError(id, -32000, "failed to focus agent");
                      return;
                    }
                    auto resultValue = toJsonValue(*result, respAlloc);
                    rpc->sendResponse(id, resultValue);
                    return;
                  }
                  if (method == kRpcAgentsCompact) {
                    params = requireObjectParams(request, kRpcAgentsCompact, id, *rpc);
                    if (!params) {
                      return;
                    }
                    auto target = agentTargetRequestFromJson(*params);
                    auto result = service_->compactAgent(target);
                    if (!result.has_value()) {
                      rpc->sendError(id, -32000, "failed to compact agent");
                      return;
                    }
                    auto resultValue = toJsonValue(*result, respAlloc);
                    rpc->sendResponse(id, resultValue);
                    return;
                  }
                  if (method == kRpcAgentsInterrupt) {
                    params = requireObjectParams(request, kRpcAgentsInterrupt, id, *rpc);
                    if (!params) {
                      return;
                    }
                    auto target = agentTargetRequestFromJson(*params);
                    auto result = service_->interruptAgent(target);
                    if (!result.has_value()) {
                      rpc->sendError(id, -32000, "failed to interrupt agent");
                      return;
                    }
                    auto resultValue = toJsonValue(*result, respAlloc);
                    rpc->sendResponse(id, resultValue);
                    return;
                  }
                  if (method == kRpcProcessesList) {
                    auto listRequest =
                        params ? processesListRequestFromJson(*params)
                               : ProcessesListRequest{};
                    auto result = toJsonValue(service_->listProcesses(listRequest),
                                              respAlloc);
                    rpc->sendResponse(id, result);
                    return;
                  }
                  if (method == kRpcProcessesGet) {
                    params = requireObjectParams(request, kRpcProcessesGet, id, *rpc);
                    if (!params) {
                      return;
                    }
                    auto getRequest = processesGetRequestFromJson(*params);
                    auto result = service_->getProcess(getRequest);
                    if (!result.has_value()) {
                      rpc->sendError(id, -32000, "process not found");
                      return;
                    }
                    auto resultValue = toJsonValue(*result, respAlloc);
                    rpc->sendResponse(id, resultValue);
                    return;
                  }
                  if (method == kRpcProcessesFocusState) {
                    auto summaryRequest =
                        params ? processesListRequestFromJson(*params)
                               : ProcessesListRequest{};
                    auto result = toJsonValue(
                        service_->focusProcessState(registeredClientId, summaryRequest),
                        respAlloc);
                    rpc->sendResponse(id, result);
                    return;
                  }
                  if (method == kRpcTranscriptGet) {
                    auto transcriptRequest =
                        params ? transcriptGetRequestFromJson(*params)
                               : TranscriptGetRequest{};
                    auto result =
                        service_->getTranscript(registeredClientId, transcriptRequest);
                    if (!result.has_value()) {
                      rpc->sendError(id, -32000, "transcript not found");
                      return;
                    }
                    auto resultValue = toJsonValue(*result, respAlloc);
                    rpc->sendResponse(id, resultValue);
                    return;
                  }
                  if (method == kRpcToolCallsList) {
                    auto toolCallsRequest =
                        params ? toolCallsListRequestFromJson(*params)
                               : ToolCallsListRequest{};
                    auto result = toJsonValue(
                        service_->listToolCalls(registeredClientId, toolCallsRequest),
                        respAlloc);
                    rpc->sendResponse(id, result);
                    return;
                  }
                  if (method == kRpcSubagentsActivity) {
                    auto subagentsRequest =
                        params ? subagentsActivityRequestFromJson(*params)
                               : SubagentsActivityRequest{};
                    auto result = toJsonValue(
                        service_->subagentActivity(registeredClientId, subagentsRequest),
                        respAlloc);
                    rpc->sendResponse(id, result);
                    return;
                  }
                  if (method == kRpcPermissionsGetMode ||
                      method == kRpcPermissionsListPending) {
                    auto permRequest =
                        params ? permissionModeRequestFromJson(*params)
                               : PermissionModeRequest{};
                    auto result = toJsonValue(
                        service_->getPermissionQueue(registeredClientId, permRequest),
                        respAlloc);
                    rpc->sendResponse(id, result);
                    return;
                  }
                  if (method == kRpcPermissionsSetMode) {
                    params = requireObjectParams(request, kRpcPermissionsSetMode, id, *rpc);
                    if (!params) {
                      return;
                    }
                    auto update = permissionModeUpdateRequestFromJson(*params);
                    auto result = toJsonValue(
                        service_->setPermissionMode(registeredClientId, update),
                        respAlloc);
                    rpc->sendResponse(id, result);
                    return;
                  }
                  if (method == kRpcPermissionsResolve) {
                    params = requireObjectParams(request, kRpcPermissionsResolve, id, *rpc);
                    if (!params) {
                      return;
                    }
                    rapidjson::Value result(service_->resolvePermission(
                        permissionResolveRequestFromJson(*params)));
                    rpc->sendResponse(id, result);
                    return;
                  }
                  if (method == kRpcModesList) {
                    auto result = toJsonValue(service_->listModes(), respAlloc);
                    rpc->sendResponse(id, result);
                    return;
                  }
                  if (method == kRpcModesGet) {
                    params = requireObjectParams(request, kRpcModesGet, id, *rpc);
                    if (!params) return;
                    auto result = service_->getMode(modesGetRequestFromJson(*params));
                    if (!result) { rpc->sendError(id, -32000, "mode not found"); return; }
                    auto jsonResult = toJsonValue(*result, respAlloc);
                    rpc->sendResponse(id, jsonResult);
                    return;
                  }
                  if (method == kRpcAgentsSetMode) {
                    params = requireObjectParams(request, kRpcAgentsSetMode, id, *rpc);
                    if (!params) return;
                    auto result = service_->setAgentMode(registeredClientId, agentsSetModeRequestFromJson(*params));
                    if (!result) { rpc->sendError(id, -32000, "failed to set mode"); return; }
                    auto jsonResult = toJsonValue(*result, respAlloc);
                    rpc->sendResponse(id, jsonResult);
                    return;
                  }
                  if (method == kRpcPersonasList) {
                    auto result = toJsonValue(service_->listPersonas(), respAlloc);
                    rpc->sendResponse(id, result);
                    return;
                  }
                  if (method == kRpcToolsCatalog) {
                    auto result = toJsonValue(service_->toolCatalog(), respAlloc);
                    rpc->sendResponse(id, result);
                    return;
                  }
                  if (method == kRpcBenchmarksListSupported) {
                    auto result = toJsonValue(service_->listSupportedBenchmarks(), respAlloc);
                    rpc->sendResponse(id, result);
                    return;
                  }
                  if (method == kRpcHooksRecentActivity) {
                    params = requireObjectParams(request, kRpcHooksRecentActivity, id, *rpc);
                    if (!params) return;
                    auto result = toJsonValue(service_->recentHookActivity(hooksRecentActivityRequestFromJson(*params)), respAlloc);
                    rpc->sendResponse(id, result);
                    return;
                  }
                  if (method == kRpcModelsList) {
                    auto result = toJsonValue(service_->listModels(false), respAlloc);
                    rpc->sendResponse(id, result);
                    return;
                  }
                  if (method == kRpcModelsRefresh) {
                    auto result = toJsonValue(service_->listModels(true), respAlloc);
                    rpc->sendResponse(id, result);
                    return;
                  }
                  if (method == kRpcModelsSwitch) {
                    params = requireObjectParams(request, kRpcModelsSwitch, id, *rpc);
                    if (!params) {
                      return;
                    }
                    auto result =
                        service_->switchModel(modelSwitchRequestFromJson(*params));
                    if (!result.has_value()) {
                      rpc->sendError(id, -32000, "failed to switch model");
                      return;
                    }
                    auto resultValue = toJsonValue(*result, respAlloc);
                    rpc->sendResponse(id, resultValue);
                    return;
                  }
                  if (method == kRpcProvidersList) {
                    auto result = toJsonValue(service_->listProviders(), respAlloc);
                    rpc->sendResponse(id, result);
                    return;
                  }
                  if (method == kRpcProvidersUpdateProfiles) {
                    params = requireObjectParams(request, kRpcProvidersUpdateProfiles, id, *rpc);
                    if (!params) {
                      return;
                    }
                    auto result = toJsonValue(
                        service_->updateProviderProfiles(
                            providerProfilesUpdateRequestFromJson(*params)),
                        respAlloc);
                    rpc->sendResponse(id, result);
                    return;
                  }
                  if (method == kRpcProvidersInvalidateModelCache) {
                    auto result = toJsonValue(service_->invalidateModelCache(),
                                              respAlloc);
                    rpc->sendResponse(id, result);
                    return;
                  }
                  if (method == kRpcConfigGet) {
                    auto result = toJsonValue(service_->getConfig(), respAlloc);
                    rpc->sendResponse(id, result);
                    return;
                  }
                  if (method == kRpcConfigUpdate) {
                    params = requireObjectParams(request, kRpcConfigUpdate, id, *rpc);
                    if (!params) {
                      return;
                    }
                    auto result = toJsonValue(
                        service_->updateConfig(configUpdateRequestFromJson(*params)),
                        respAlloc);
                    rpc->sendResponse(id, result);
                    return;
                  }
                  if (method == kRpcHistoryGet) {
                    auto historyRequest =
                        params ? historyGetRequestFromJson(*params)
                               : HistoryGetRequest{};
                    auto result = toJsonValue(
                        service_->getHistory(registeredClientId, historyRequest),
                        respAlloc);
                    rpc->sendResponse(id, result);
                    return;
                  }
                  if (method == kRpcHistoryUndo ||
                      method == kRpcHistoryUndoTranscript) {
                    params = requireObjectParams(request, method.c_str(), id, *rpc);
                    if (!params) {
                      return;
                    }
                    auto historyRequest = historyUndoRequestFromJson(*params);
                    auto result = toJsonValue(
                        service_->undoHistory(registeredClientId, historyRequest),
                        respAlloc);
                    rpc->sendResponse(id, result);
                    return;
                  }
                  if (method == kRpcHistoryRedo ||
                      method == kRpcHistoryRedoTranscript) {
                    params = requireObjectParams(request, method.c_str(), id, *rpc);
                    if (!params) {
                      return;
                    }
                    auto historyRequest = historyRedoRequestFromJson(*params);
                    auto result = toJsonValue(
                        service_->redoHistory(registeredClientId, historyRequest),
                        respAlloc);
                    rpc->sendResponse(id, result);
                    return;
                  }
                  if (method == kRpcRouterGet) {
                    auto result = toJsonValue(service_->getRouterConfig(), respAlloc);
                    rpc->sendResponse(id, result);
                    return;
                  }
                  if (method == kRpcRouterUpdate) {
                    params = requireObjectParams(request, kRpcRouterUpdate, id, *rpc);
                    if (!params) {
                      return;
                    }
                    auto result = toJsonValue(
                        service_->updateRouterConfig(
                            routerConfigUpdateRequestFromJson(*params)),
                        respAlloc);
                    rpc->sendResponse(id, result);
                    return;
                  }
                  if (method == kRpcPurposesGet) {
                    auto result = toJsonValue(service_->getPurposesConfig(), respAlloc);
                    rpc->sendResponse(id, result);
                    return;
                  }
                  if (method == kRpcPurposesUpdate) {
                    params = requireObjectParams(request, kRpcPurposesUpdate, id, *rpc);
                    if (!params) {
                      return;
                    }
                    auto result = toJsonValue(
                        service_->updatePurposesConfig(
                            purposesConfigUpdateRequestFromJson(*params)),
                        respAlloc);
                    rpc->sendResponse(id, result);
                    return;
                  }
                  if (method == kRpcRollingMemoryGet) {
                    auto result =
                        toJsonValue(service_->getRollingMemoryConfig(), respAlloc);
                    rpc->sendResponse(id, result);
                    return;
                  }
                  if (method == kRpcRollingMemoryUpdate) {
                    params =
                        requireObjectParams(request, kRpcRollingMemoryUpdate, id, *rpc);
                    if (!params) {
                      return;
                    }
                    auto result = toJsonValue(
                        service_->updateRollingMemoryConfig(
                            rollingMemoryConfigUpdateRequestFromJson(*params)),
                        respAlloc);
                    rpc->sendResponse(id, result);
                    return;
                  }
                  if (method == kRpcMcpGet) {
                    auto result = toJsonValue(service_->getMcpConfig(), respAlloc);
                    rpc->sendResponse(id, result);
                    return;
                  }
                  if (method == kRpcMcpUpdate) {
                    params = requireObjectParams(request, kRpcMcpUpdate, id, *rpc);
                    if (!params) {
                      return;
                    }
                    auto result = toJsonValue(
                        service_->updateMcpConfig(
                            mcpConfigUpdateRequestFromJson(*params)),
                        respAlloc);
                    rpc->sendResponse(id, result);
                    return;
                  }
                  if (method == kRpcAccountsList) {
                    params = requireObjectParams(request, kRpcAccountsList, id, *rpc);
                    if (!params) {
                      return;
                    }
                    auto result =
                        toJsonValue(service_->listAccounts(accountsRequestFromJson(*params)),
                                    respAlloc);
                    rpc->sendResponse(id, result);
                    return;
                  }
                  if (method == kRpcAccountsDelete) {
                    params = requireObjectParams(request, kRpcAccountsDelete, id, *rpc);
                    if (!params) {
                      return;
                    }
                    rapidjson::Value result(service_->deleteAccount(
                        accountDeleteRequestFromJson(*params)));
                    rpc->sendResponse(id, result);
                    return;
                  }
                  if (method == kRpcQuotasGet || method == kRpcQuotasGetCached) {
                    params = requireObjectParams(request, method.c_str(), id, *rpc);
                    if (!params) {
                      return;
                    }
                    auto result = toJsonValue(
                        service_->getQuotas(quotasRequestFromJson(*params),
                                            method == kRpcQuotasGet),
                        respAlloc);
                    rpc->sendResponse(id, result);
                    return;
                  }
                  if (method == kRpcHooksList) {
                    auto result = toJsonValue(service_->listHooks(), respAlloc);
                    rpc->sendResponse(id, result);
                    return;
                  }
                  if (method == kRpcHooksReload) {
                    auto result = toJsonValue(service_->reloadHooks(), respAlloc);
                    rpc->sendResponse(id, result);
                    return;
                  }
                  if (method == kRpcHooksState) {
                    auto stateRequest =
                        params ? hooksStateRequestFromJson(*params)
                               : HooksStateRequest{};
                    auto result = toJsonValue(service_->hookState(stateRequest),
                                              respAlloc);
                    rpc->sendResponse(id, result);
                    return;
                  }
                  if (method == kRpcPactsList) {
                    auto pactsRequest =
                        params ? pactsListRequestFromJson(*params)
                               : PactsListRequest{};
                    auto result = toJsonValue(
                        service_->listPacts(registeredClientId, pactsRequest),
                        respAlloc);
                    rpc->sendResponse(id, result);
                    return;
                  }
                  if (method == kRpcPactsGet) {
                    params = requireObjectParams(request, kRpcPactsGet, id, *rpc);
                    if (!params) {
                      return;
                    }
                    auto pact = service_->getPact(registeredClientId,
                                                  pactsGetRequestFromJson(*params));
                    if (!pact.has_value()) {
                      rpc->sendError(id, -32000, "pact not found");
                      return;
                    }
                    auto resultValue = toJsonValue(*pact, respAlloc);
                    rpc->sendResponse(id, resultValue);
                    return;
                  }
                  if (method == kRpcWorkflowsList) {
                    auto result = toJsonValue(service_->listWorkflows(), respAlloc);
                    rpc->sendResponse(id, result);
                    return;
                  }
                  if (method == kRpcWorkflowsExecute) {
                    params = requireObjectParams(request, kRpcWorkflowsExecute, id, *rpc);
                    if (!params) {
                      return;
                    }
                    rapidjson::Value result(service_->executeWorkflow(
                        workflowExecuteRequestFromJson(*params)));
                    rpc->sendResponse(id, result);
                    return;
                  }
                  if (method == kRpcEditsList) {
                    auto editsRequest =
                        params ? editsListRequestFromJson(*params)
                               : EditsListRequest{};
                    auto result = toJsonValue(
                        service_->listEdits(registeredClientId, editsRequest),
                        respAlloc);
                    rpc->sendResponse(id, result);
                    return;
                  }
                  if (method == kRpcEditsUndo) {
                    params = requireObjectParams(request, kRpcEditsUndo, id, *rpc);
                    if (!params) {
                      return;
                    }
                    auto editsRequest = editsUndoRequestFromJson(*params);
                    auto result = toJsonValue(
                        service_->undoEdit(registeredClientId, editsRequest),
                        respAlloc);
                    rpc->sendResponse(id, result);
                    return;
                  }
                  if (method == kRpcEditsRedo) {
                    params = requireObjectParams(request, kRpcEditsRedo, id, *rpc);
                    if (!params) {
                      return;
                    }
                    auto editsRequest = editsRedoRequestFromJson(*params);
                    auto result = toJsonValue(
                        service_->redoEdit(registeredClientId, editsRequest),
                        respAlloc);
                    rpc->sendResponse(id, result);
                    return;
                  }
                  if (method == kRpcArtifactsList) {
                    ThreadsOpenRequest artifactsRequest{
                        params && params->HasMember("thread_id")
                            ? (*params)["thread_id"].GetString()
                            : ""};
                    auto result = toJsonValue(
                        service_->listArtifacts(registeredClientId, artifactsRequest),
                        respAlloc);
                    rpc->sendResponse(id, result);
                    return;
                  }
                  if (method == kRpcEventsSubscribe) {
                    params = requireObjectParams(request, kRpcEventsSubscribe, id, *rpc);
                    if (!params) {
                      return;
                    }
                    auto sub = eventSubscriptionRequestFromJson(*params);
                    auto result = toJsonValue(
                        service_->subscribe(
                            registeredClientId, sub,
                            [weakRpc](const DaemonEventEnvelope &event) {
                              auto notificationRpc = weakRpc.lock();
                              if (!notificationRpc || !notificationRpc->isRunning()) {
                                return;
                              }
                              rapidjson::Document doc;
                              doc.SetObject();
                              auto &a = doc.GetAllocator();
                              auto payload = toJsonValue(event, a);
                              notificationRpc->sendNotification(kNotificationDaemonEvent, payload);
                            }),
                        respAlloc);
                    rpc->sendResponse(id, result);
                    return;
                  }
                  if (method == kRpcEventsUnsubscribe) {
                    auto result = toJsonValue(service_->unsubscribe(registeredClientId), respAlloc);
                    rpc->sendResponse(id, result);
                    return;
                  }
                  if (method == kRpcClientsList) {
                    auto result = toJsonValue(service_->listClients(), respAlloc);
                    rpc->sendResponse(id, result);
                    return;
                  }
                  if (method == kRpcBenchmarksStart) {
                    params = requireObjectParams(request, kRpcBenchmarksStart, id, *rpc);
                    if (!params) {
                      return;
                    }
                    auto startRequest = benchmarksStartRequestFromJson(*params);
                    auto result =
                        toJsonValue(service_->startBenchmark(registeredClientId, startRequest),
                                    respAlloc);
                    rpc->sendResponse(id, result);
                    return;
                  }
                  if (method == kRpcBenchmarksStatus) {
                    params = requireObjectParams(request, kRpcBenchmarksStatus, id, *rpc);
                    if (!params) {
                      return;
                    }
                    auto statusRequest = benchmarksStatusRequestFromJson(*params);
                    auto result =
                        toJsonValue(service_->getBenchmarkStatus(registeredClientId, statusRequest),
                                    respAlloc);
                    rpc->sendResponse(id, result);
                    return;
                  }
                  if (method == kRpcBenchmarksLogs) {
                    params = requireObjectParams(request, kRpcBenchmarksLogs, id, *rpc);
                    if (!params) {
                      return;
                    }
                    auto logsRequest = benchmarksLogsRequestFromJson(*params);
                    auto result =
                        toJsonValue(service_->getBenchmarkLogs(registeredClientId, logsRequest),
                                    respAlloc);
                    rpc->sendResponse(id, result);
                    return;
                  }
                  rpc->sendError(id, -32601, "unknown daemon RPC method");
                } catch (const std::exception &e) {
                  rpc->sendError(id, -32000, e.what());
                }
              });
          connection->rpc->start();
          while (!stopToken.stop_requested() && connection->rpc->isRunning()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
          }
          connection->rpc->stop();
          if (!registeredClientId.empty() && service_) {
            service_->unsubscribe(registeredClientId);
            service_->unregisterClient(registeredClientId);
          }
          connection->finished.store(true);
        });

    {
      std::lock_guard<std::mutex> lock(clientsMutex_);
      clients_.push_back(connection);
    }
    reapFinishedClients();
  }
}

void DaemonServer::stopClients() {
  std::vector<std::shared_ptr<ClientConnection>> clients;
  {
    std::lock_guard<std::mutex> lock(clientsMutex_);
    clients.swap(clients_);
  }
  for (auto &client : clients) {
    if (client->rpc) {
      client->rpc->stop();
    }
    if (client->thread.joinable()) {
      client->thread.request_stop();
    }
  }
  clients.clear();
}

void DaemonServer::reapFinishedClients() {
  std::lock_guard<std::mutex> lock(clientsMutex_);
  auto it = clients_.begin();
  while (it != clients_.end()) {
    if ((*it)->finished.load()) {
      if ((*it)->rpc) {
        (*it)->rpc->stop();
      }
      if ((*it)->thread.joinable()) {
        (*it)->thread.request_stop();
      }
      it = clients_.erase(it);
      continue;
    }
    ++it;
  }
}

} // namespace firmius::daemon

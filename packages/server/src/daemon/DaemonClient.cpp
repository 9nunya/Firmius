#include "daemon/DaemonClient.hpp"

#include "daemon/ProtocolSerialization.hpp"
#include "daemon/SocketTransport.hpp"

#include <filesystem>
#include <fstream>
#include <rapidjson/document.h>
#include <stdexcept>
#include <thread>

#if defined(_WIN32)
#include <windows.h>
#else
#include <signal.h>
#include <spawn.h>
#include <sys/types.h>
#include <unistd.h>
extern char **environ;
#endif

namespace firmius::daemon {

namespace {

rapidjson::Document makeParams() {
  rapidjson::Document params;
  params.SetObject();
  return params;
}

template <typename Request>
rapidjson::Document makeParams(const Request &request) {
  rapidjson::Document params;
  params.SetObject();
  auto value = toJsonValue(request, params.GetAllocator());
  params.CopyFrom(value, params.GetAllocator());
  return params;
}

bool boolResult(const rapidjson::Value &value, const char *field) {
  return value.IsObject() && value.HasMember(field) && value[field].IsBool() &&
         value[field].GetBool();
}

bool acceptedOrSnapshotResult(const rapidjson::Value &value,
                              const char *field = "accepted") {
  if (boolResult(value, field)) {
    return true;
  }
  return value.IsObject() && value.HasMember("agent_id") &&
         value["agent_id"].IsString();
}

rapidjson::Document makeThreadParams(const std::string &threadId) {
  rapidjson::Document params;
  params.SetObject();
  params.AddMember("thread_id",
                   rapidjson::Value(threadId.c_str(), params.GetAllocator()).Move(),
                   params.GetAllocator());
  return params;
}

} // namespace

DaemonClient::DaemonClient(DaemonClientOptions options) : options_(std::move(options)) {}
DaemonClient::~DaemonClient() { disconnect(); }

bool DaemonClient::connect() {
  try {
    transport();
  } catch (const std::exception &) {
    if (!options_.autoStart) {
      throw;
    }
    spawnDaemon();
    if (!waitForConnectReady()) {
      return false;
    }
  }

  auto params = makeParams();
  auto helloValue = toJsonValue(
      ClientHelloRequest{kProtocolVersion, options_.identity, options_.presence, "", ""},
      params.GetAllocator());
  params.CopyFrom(helloValue, params.GetAllocator());
  auto response = transport().sendRequest(kRpcClientHello, params, 3000);
  session_ = clientHelloResponseFromJson(response["result"]).session;
  connected_ = true;
  return true;
}

void DaemonClient::disconnect() {
  if (!transport_) {
    connected_ = false;
    return;
  }
  try {
    auto params = makeParams();
    params.AddMember("clientId",
                     rapidjson::Value(options_.identity.clientId.c_str(),
                                      params.GetAllocator())
                         .Move(),
                     params.GetAllocator());
    (void)transport_->sendRequest(kRpcClientGoodbye, params, 1000);
  } catch (...) {
  }
  transport_->stop();
  transport_.reset();
  connected_ = false;
  subscribed_ = false;
}

bool DaemonClient::connected() const { return connected_; }

UiSnapshot DaemonClient::uiSnapshot(const UiSnapshotRequest &request) const {
  auto params = makeParams(request);
  // 30s timeout — server-side waitForReady() also uses 30s, and fresh daemon
  // init can take several seconds (provider hydration, model enumeration, etc.)
  auto response = transport().sendRequest(kRpcUiSnapshotGet, params, 30000);
  return uiSnapshotFromJson(response["result"]);
}

DaemonPingResponse DaemonClient::ping() const {
  auto params = makeParams();
  auto response = transport().sendRequest(kRpcDaemonPing, params, 2000);
  return daemonPingResponseFromJson(response["result"]);
}

ClientSessionSnapshot DaemonClient::session() const { return session_; }

std::vector<firmius::shared::ThreadMetadata> DaemonClient::listThreads() const {
  auto params = makeParams();
  auto response = transport().sendRequest(kRpcThreadsList, params, 3000);
  return threadMetadataListFromJson(response["result"]);
}

std::vector<ThreadOverview>
DaemonClient::listThreadOverviews(const ThreadOverviewRequest &request) const {
  auto params = makeParams(request);
  auto response = transport().sendRequest(kRpcThreadsOverview, params, 3000);
  return threadOverviewListFromJson(response["result"]);
}

ThreadSnapshot DaemonClient::getThread(const ThreadsOpenRequest &request) const {
  auto params = makeThreadParams(request.threadId);
  auto response = transport().sendRequest(kRpcThreadsGet, params, 3000);
  return threadSnapshotFromJson(response["result"]);
}

ThreadsOpenResponse DaemonClient::openThread(const std::string &threadId) const {
  return openThread(ThreadsOpenRequest{threadId});
}

ThreadsOpenResponse
DaemonClient::openThread(const ThreadsOpenRequest &request) const {
  auto params = makeThreadParams(request.threadId);
  auto response = transport().sendRequest(kRpcThreadsOpen, params, 3000);
  return threadsOpenResponseFromJson(response["result"]);
}

ThreadSnapshot DaemonClient::focusThread(const ThreadsOpenRequest &request) const {
  auto params = makeThreadParams(request.threadId);
  auto response = transport().sendRequest(kRpcThreadsFocus, params, 3000);
  return threadSnapshotFromJson(response["result"]);
}

ThreadsCreateResponse DaemonClient::createThread(
    const ThreadsCreateRequest &request) const {
  auto params = makeParams(request);
  auto response = transport().sendRequest(kRpcThreadsCreate, params, 3000);
  return threadsCreateResponseFromJson(response["result"]);
}

ThreadsSendResponse DaemonClient::send(const ThreadsSendRequest &request) const {
  auto params = makeParams(request);
  auto response = transport().sendRequest(kRpcThreadsSend, params, 3000);
  return threadsSendResponseFromJson(response["result"]);
}

std::optional<TranscriptSnapshot>
DaemonClient::getTranscript(const TranscriptGetRequest &request) const {
  auto params = makeParams(request);
  auto response = transport().sendRequest(kRpcTranscriptGet, params, 3000);
  if (!response.HasMember("result")) {
    return std::nullopt;
  }
  return transcriptSnapshotFromJson(response["result"]);
}

std::vector<ToolCallSnapshot>
DaemonClient::listToolCalls(const ToolCallsListRequest &request) const {
  auto params = makeParams(request);
  auto response = transport().sendRequest(kRpcToolCallsList, params, 3000);
  return toolCallSnapshotListFromJson(response["result"]);
}

SubagentActivitySnapshot
DaemonClient::subagentActivity(const SubagentsActivityRequest &request) const {
  auto params = makeParams(request);
  auto response = transport().sendRequest(kRpcSubagentsActivity, params, 3000);
  return subagentActivitySnapshotFromJson(response["result"]);
}

std::vector<ProcessSnapshot>
DaemonClient::listProcesses(const ProcessesListRequest &request) const {
  auto params = makeParams(request);
  auto response = transport().sendRequest(kRpcProcessesList, params, 3000);
  return processSnapshotListFromJson(response["result"]);
}

std::optional<ProcessSnapshot>
DaemonClient::getProcess(const ProcessesGetRequest &request) const {
  auto params = makeParams(request);
  auto response = transport().sendRequest(kRpcProcessesGet, params, 3000);
  if (!response.HasMember("result")) {
    return std::nullopt;
  }
  return processSnapshotFromJson(response["result"]);
}

ProcessRuntimeSummary
DaemonClient::focusProcessState(const ProcessesListRequest &request) const {
  auto params = makeParams(request);
  auto response = transport().sendRequest(kRpcProcessesFocusState, params, 3000);
  return processRuntimeSummaryFromJson(response["result"]);
}

ArtifactCatalogSnapshot
DaemonClient::listArtifacts(const ThreadsOpenRequest &request) const {
  auto params = makeThreadParams(request.threadId);
  auto response = transport().sendRequest(kRpcArtifactsList, params, 3000);
  return artifactCatalogSnapshotFromJson(response["result"]);
}

HistorySnapshot DaemonClient::getHistory(const HistoryGetRequest &request) const {
  auto params = makeParams(request);
  auto response = transport().sendRequest(kRpcHistoryGet, params, 3000);
  return historySnapshotFromJson(response["result"]);
}

HistoryMutationResult
DaemonClient::undoHistory(const HistoryUndoRequest &request) const {
  auto params = makeParams(request);
  auto response = transport().sendRequest(kRpcHistoryUndo, params, 3000);
  return historyMutationResultFromJson(response["result"]);
}

HistoryMutationResult
DaemonClient::redoHistory(const HistoryRedoRequest &request) const {
  auto params = makeParams(request);
  auto response = transport().sendRequest(kRpcHistoryRedo, params, 3000);
  return historyMutationResultFromJson(response["result"]);
}

HistoryMutationResult
DaemonClient::undoTranscript(const HistoryUndoRequest &request) const {
  auto params = makeParams(request);
  auto response = transport().sendRequest(kRpcHistoryUndoTranscript, params, 3000);
  return historyMutationResultFromJson(response["result"]);
}

HistoryMutationResult
DaemonClient::redoTranscript(const HistoryRedoRequest &request) const {
  auto params = makeParams(request);
  auto response = transport().sendRequest(kRpcHistoryRedoTranscript, params, 3000);
  return historyMutationResultFromJson(response["result"]);
}

EditHistorySnapshot DaemonClient::listEdits(const EditsListRequest &request) const {
  auto params = makeParams(request);
  auto response = transport().sendRequest(kRpcEditsList, params, 3000);
  return editHistorySnapshotFromJson(response["result"]);
}

EditMutationResult DaemonClient::undoEdit(const EditsUndoRequest &request) const {
  auto params = makeParams(request);
  auto response = transport().sendRequest(kRpcEditsUndo, params, 3000);
  return editMutationResultFromJson(response["result"]);
}

EditMutationResult DaemonClient::redoEdit(const EditsRedoRequest &request) const {
  auto params = makeParams(request);
  auto response = transport().sendRequest(kRpcEditsRedo, params, 3000);
  return editMutationResultFromJson(response["result"]);
}

PermissionQueueSnapshot DaemonClient::getPermissionMode(
    const PermissionModeRequest &request) const {
  auto params = makeParams(request);
  auto response = transport().sendRequest(kRpcPermissionsGetMode, params, 3000);
  return permissionQueueSnapshotFromJson(response["result"]);
}

PermissionQueueSnapshot DaemonClient::setPermissionMode(
    const PermissionModeUpdateRequest &request) const {
  auto params = makeParams(request);
  auto response = transport().sendRequest(kRpcPermissionsSetMode, params, 3000);
  return permissionQueueSnapshotFromJson(response["result"]);
}

PermissionQueueSnapshot DaemonClient::listPendingPermissions(
    const PermissionModeRequest &request) const {
  auto params = makeParams(request);
  auto response = transport().sendRequest(kRpcPermissionsListPending, params, 3000);
  return permissionQueueSnapshotFromJson(response["result"]);
}

bool DaemonClient::resolvePermission(
    const PermissionResolveRequest &request) const {
  auto params = makeParams(request);
  auto response = transport().sendRequest(kRpcPermissionsResolve, params, 3000);
  return boolResult(response["result"], "resolved");
}

std::vector<ClientSessionSnapshot> DaemonClient::listClients() const {
  auto params = makeParams();
  auto response = transport().sendRequest(kRpcClientsList, params, 3000);
  return clientSessionSnapshotListFromJson(response["result"]);
}

AgentTreeSnapshot DaemonClient::listAgents(const AgentTargetRequest &request) const {
  auto params = makeParams(request);
  auto response = transport().sendRequest(kRpcAgentsList, params, 3000);
  return agentTreeSnapshotFromJson(response["result"]);
}

std::optional<AgentRuntimeSnapshot>
DaemonClient::getAgent(const AgentTargetRequest &request) const {
  auto params = makeParams(request);
  auto response = transport().sendRequest(kRpcAgentsGet, params, 3000);
  if (!response.HasMember("result")) {
    return std::nullopt;
  }
  return agentRuntimeSnapshotFromJson(response["result"]);
}

std::optional<AgentTodoSnapshot>
DaemonClient::getAgentTodo(const AgentTargetRequest &request) const {
  auto params = makeParams(request);
  auto response = transport().sendRequest(kRpcAgentTodoGet, params, 3000);
  if (!response.HasMember("result")) {
    return std::nullopt;
  }
  return agentTodoSnapshotFromJson(response["result"]);
}

std::optional<AgentRuntimeSnapshot>
DaemonClient::focusAgent(const AgentTargetRequest &request) const {
  auto params = makeParams(request);
  auto response = transport().sendRequest(kRpcAgentsFocus, params, 3000);
  if (!response.HasMember("result")) {
    return std::nullopt;
  }
  return agentRuntimeSnapshotFromJson(response["result"]);
}

bool DaemonClient::compactAgent(const AgentTargetRequest &request) const {
  auto params = makeParams(request);
  auto response = transport().sendRequest(kRpcAgentsCompact, params, 3000);
  if (!response.HasMember("result")) {
    return false;
  }
  return acceptedOrSnapshotResult(response["result"]);
}

bool DaemonClient::interruptAgent(const AgentTargetRequest &request) const {
  auto params = makeParams(request);
  auto response = transport().sendRequest(kRpcAgentsInterrupt, params, 3000);
  if (!response.HasMember("result")) {
    return false;
  }
  return acceptedOrSnapshotResult(response["result"]);
}

bool DaemonClient::abortAndFlushQueuedMessages(
    const AgentTargetRequest &request) const {
  auto params = makeParams(request);
  auto response = transport().sendRequest(
      kRpcAgentsAbortAndFlushQueuedMessages, params, 3000);
  if (!response.HasMember("result")) {
    return false;
  }
  return acceptedOrSnapshotResult(response["result"]);
}

std::optional<AgentRuntimeSnapshot>
DaemonClient::setAgentMode(const AgentsSetModeRequest &request) const {
  auto params = makeParams(request);
  auto response = transport().sendRequest(kRpcAgentsSetMode, params, 3000);
  if (!response.HasMember("result")) {
    return std::nullopt;
  }
  return agentRuntimeSnapshotFromJson(response["result"]);
}

ModelCatalogSnapshot DaemonClient::listModels(bool refresh) const {
  auto params = makeParams();
  params.AddMember("refresh", refresh, params.GetAllocator());
  auto response = transport().sendRequest(kRpcModelsList, params, 3000);
  return modelCatalogSnapshotFromJson(response["result"]);
}

ModelCatalogSnapshot DaemonClient::refreshModels() const {
  auto params = makeParams();
  auto response = transport().sendRequest(kRpcModelsRefresh, params, 3000);
  return modelCatalogSnapshotFromJson(response["result"]);
}

std::optional<AgentRuntimeSnapshot>
DaemonClient::switchModel(const ModelSwitchRequest &request) const {
  auto params = makeParams(request);
  auto response = transport().sendRequest(kRpcModelsSwitch, params, 3000);
  if (!response.HasMember("result")) {
    return std::nullopt;
  }
  return agentRuntimeSnapshotFromJson(response["result"]);
}

ProviderCatalogSnapshot DaemonClient::listProviders() const {
  auto params = makeParams();
  auto response = transport().sendRequest(kRpcProvidersList, params, 3000);
  return providerCatalogSnapshotFromJson(response["result"]);
}

ProviderCatalogSnapshot DaemonClient::updateProviderProfiles(
    const ProviderProfilesUpdateRequest &request) const {
  auto params = makeParams(request);
  auto response = transport().sendRequest(kRpcProvidersUpdateProfiles, params, 3000);
  return providerCatalogSnapshotFromJson(response["result"]);
}

ModelCatalogSnapshot DaemonClient::invalidateModelCache() const {
  auto params = makeParams();
  auto response = transport().sendRequest(kRpcProvidersInvalidateModelCache, params, 3000);
  return modelCatalogSnapshotFromJson(response["result"]);
}

std::vector<AccountSnapshot>
DaemonClient::listAccounts(const AccountsRequest &request) const {
  auto params = makeParams(request);
  auto response = transport().sendRequest(kRpcAccountsList, params, 3000);
  return accountSnapshotListFromJson(response["result"]);
}

bool DaemonClient::deleteAccount(const AccountDeleteRequest &request) const {
  auto params = makeParams(request);
  auto response = transport().sendRequest(kRpcAccountsDelete, params, 3000);
  return boolResult(response["result"], "deleted");
}

QuotaSnapshot DaemonClient::getQuotas(const QuotasRequest &request) const {
  auto params = makeParams(request);
  auto response = transport().sendRequest(kRpcQuotasGet, params, 3000);
  return quotaSnapshotFromJson(response["result"]);
}

QuotaSnapshot DaemonClient::getCachedQuotas(const QuotasRequest &request) const {
  auto params = makeParams(request);
  auto response = transport().sendRequest(kRpcQuotasGetCached, params, 3000);
  return quotaSnapshotFromJson(response["result"]);
}

UserConfigSnapshot DaemonClient::getConfig() const {
  auto params = makeParams();
  auto response = transport().sendRequest(kRpcConfigGet, params, 3000);
  return userConfigSnapshotFromJson(response["result"]);
}

UserConfigSnapshot
DaemonClient::updateConfig(const ConfigUpdateRequest &request) const {
  auto params = makeParams(request);
  auto response = transport().sendRequest(kRpcConfigUpdate, params, 3000);
  return userConfigSnapshotFromJson(response["result"]);
}

RouterConfigSnapshot DaemonClient::getRouterConfig() const {
  auto params = makeParams();
  auto response = transport().sendRequest(kRpcRouterGet, params, 3000);
  return routerConfigSnapshotFromJson(response["result"]);
}

RouterConfigSnapshot DaemonClient::updateRouterConfig(
    const RouterConfigUpdateRequest &request) const {
  auto params = makeParams(request);
  auto response = transport().sendRequest(kRpcRouterUpdate, params, 3000);
  return routerConfigSnapshotFromJson(response["result"]);
}

PurposesConfigSnapshot DaemonClient::getPurposesConfig() const {
  auto params = makeParams();
  auto response = transport().sendRequest(kRpcPurposesGet, params, 3000);
  return purposesConfigSnapshotFromJson(response["result"]);
}

PurposesConfigSnapshot DaemonClient::updatePurposesConfig(
    const PurposesConfigUpdateRequest &request) const {
  auto params = makeParams(request);
  auto response = transport().sendRequest(kRpcPurposesUpdate, params, 3000);
  return purposesConfigSnapshotFromJson(response["result"]);
}

McpConfigSnapshot DaemonClient::getMcpConfig() const {
  auto params = makeParams();
  auto response = transport().sendRequest(kRpcMcpGet, params, 3000);
  return mcpConfigSnapshotFromJson(response["result"]);
}

McpConfigSnapshot
DaemonClient::updateMcpConfig(const McpConfigUpdateRequest &request) const {
  auto params = makeParams(request);
  auto response = transport().sendRequest(kRpcMcpUpdate, params, 3000);
  return mcpConfigSnapshotFromJson(response["result"]);
}

HookStatusSnapshot DaemonClient::listHooks() const {
  auto params = makeParams();
  auto response = transport().sendRequest(kRpcHooksList, params, 3000);
  return hookStatusSnapshotFromJson(response["result"]);
}

HookStatusSnapshot DaemonClient::reloadHooks() const {
  auto params = makeParams();
  auto response = transport().sendRequest(kRpcHooksReload, params, 3000);
  return hookStatusSnapshotFromJson(response["result"]);
}

HookStateSnapshot DaemonClient::hookState(
    const HooksStateRequest &request) const {
  auto params = makeParams(request);
  auto response = transport().sendRequest(kRpcHooksState, params, 3000);
  return hookStateSnapshotFromJson(response["result"]);
}

HooksRecentActivitySnapshot DaemonClient::recentHookActivity(
    const HooksRecentActivityRequest &request) const {
  auto params = makeParams(request);
  auto response = transport().sendRequest(kRpcHooksRecentActivity, params, 3000);
  return hooksRecentActivitySnapshotFromJson(response["result"]);
}

std::vector<PactSnapshot>
DaemonClient::listPacts(const PactsListRequest &request) const {
  auto params = makeParams(request);
  auto response = transport().sendRequest(kRpcPactsList, params, 3000);
  std::vector<PactSnapshot> pacts;
  const auto &result = response["result"];
  if (result.IsArray()) {
    pacts.reserve(result.Size());
    for (const auto &entry : result.GetArray()) {
      pacts.push_back(pactSnapshotFromJson(entry));
    }
  }
  return pacts;
}

std::optional<PactSnapshot> DaemonClient::getPact(
    const PactsGetRequest &request) const {
  auto params = makeParams(request);
  auto response = transport().sendRequest(kRpcPactsGet, params, 3000);
  if (!response.HasMember("result")) {
    return std::nullopt;
  }
  return pactSnapshotFromJson(response["result"]);
}

std::vector<WorkflowExecutionSnapshot> DaemonClient::listWorkflows() const {
  auto params = makeParams();
  auto response = transport().sendRequest(kRpcWorkflowsList, params, 3000);
  std::vector<WorkflowExecutionSnapshot> workflows;
  const auto &result = response["result"];
  if (result.IsArray()) {
    workflows.reserve(result.Size());
    for (const auto &entry : result.GetArray()) {
      workflows.push_back(workflowExecutionSnapshotFromJson(entry));
    }
  }
  return workflows;
}

bool DaemonClient::executeWorkflow(
    const WorkflowExecuteRequest &request) const {
  auto params = makeParams(request);
  auto response = transport().sendRequest(kRpcWorkflowsExecute, params, 3000);
  const auto &result = response["result"];
  return result.IsBool() ? result.GetBool() : boolResult(result, "accepted");
}

ModeCatalogSnapshot DaemonClient::listModes() const {
  auto params = makeParams(ModesListRequest{});
  auto response = transport().sendRequest(kRpcModesList, params, 3000);
  return modeCatalogSnapshotFromJson(response["result"]);
}

std::optional<ModeSnapshot> DaemonClient::getMode(
    const ModesGetRequest &request) const {
  auto params = makeParams(request);
  auto response = transport().sendRequest(kRpcModesGet, params, 3000);
  if (!response.HasMember("result")) {
    return std::nullopt;
  }
  return modeSnapshotFromJson(response["result"]);
}

PersonaCatalogSnapshot DaemonClient::listPersonas() const {
  auto params = makeParams(PersonasListRequest{});
  auto response = transport().sendRequest(kRpcPersonasList, params, 3000);
  return personaCatalogSnapshotFromJson(response["result"]);
}

ToolCatalogSnapshot DaemonClient::toolCatalog() const {
  auto params = makeParams(ToolsCatalogRequest{});
  auto response = transport().sendRequest(kRpcToolsCatalog, params, 3000);
  return toolCatalogSnapshotFromJson(response["result"]);
}

BenchmarkCatalogSnapshot DaemonClient::listSupportedBenchmarks() const {
  auto params = makeParams(BenchmarksListSupportedRequest{});
  auto response = transport().sendRequest(kRpcBenchmarksListSupported, params, 3000);
  return benchmarkCatalogSnapshotFromJson(response["result"]);
}

BenchmarksStartResponse DaemonClient::startBenchmark(
    const BenchmarksStartRequest &request) const {
  auto params = makeParams(request);
  auto response = transport().sendRequest(kRpcBenchmarksStart, params, 3000);
  return benchmarksStartResponseFromJson(response["result"]);
}

BenchmarkStatusSnapshot DaemonClient::getBenchmarkStatus(
    const BenchmarksStatusRequest &request) const {
  auto params = makeParams(request);
  auto response = transport().sendRequest(kRpcBenchmarksStatus, params, 3000);
  return benchmarkStatusSnapshotFromJson(response["result"]);
}

BenchmarkLogsSnapshot DaemonClient::getBenchmarkLogs(
    const BenchmarksLogsRequest &request) const {
  auto params = makeParams(request);
  auto response = transport().sendRequest(kRpcBenchmarksLogs, params, 3000);
  return benchmarkLogsSnapshotFromJson(response["result"]);
}

bool DaemonClient::subscribe(DaemonEventListener listener,
                             const EventSubscriptionRequest &request) {
  listener_ = std::move(listener);
  transport().setNotificationHandler([this](const rapidjson::Document &notification) {
    if (!notification.HasMember("method") ||
        std::string(notification["method"].GetString()) != kNotificationDaemonEvent) {
      return;
    }
    if (!notification.HasMember("params")) {
      return;
    }
    if (listener_) {
      const auto envelope = daemonEventEnvelopeFromJson(notification["params"]);
      listener_(envelope);
    }
  });
  auto params = makeParams(request);
  auto response = transport().sendRequest(kRpcEventsSubscribe, params, 2000);
  subscribed_ = eventSubscriptionResponseFromJson(response["result"]).subscribed;
  return subscribed_;
}

bool DaemonClient::unsubscribe() {
  auto params = makeParams();
  auto response = transport().sendRequest(kRpcEventsUnsubscribe, params, 2000);
  subscribed_ = eventSubscriptionResponseFromJson(response["result"]).subscribed;
  return !subscribed_;
}

std::string DaemonClient::daemonCommand() const {
  if (!options_.daemonExecutablePath.empty()) {
    return options_.daemonExecutablePath;
  }
#if defined(_WIN32)
  return "firmiusd.exe";
#else
  return "firmiusd";
#endif
}

bool DaemonClient::waitForConnectReady() {
  for (int attempt = 0; attempt < 50; ++attempt) {
    try {
      transport();
      return true;
    } catch (...) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100 + attempt * 10));
    }
  }
  return false;
}

void DaemonClient::spawnDaemon() const {
  const std::string command = daemonCommand();
#if defined(_WIN32)
  STARTUPINFOA si{};
  si.cb = sizeof(si);
  PROCESS_INFORMATION pi{};
  std::string cmd = "\"" + command + "\" --endpoint \"" + options_.connection.endpoint + "\"";
  std::vector<char> cmdline(cmd.begin(), cmd.end());
  cmdline.push_back('\0');
  if (!CreateProcessA(nullptr, cmdline.data(), nullptr, nullptr, FALSE,
                      DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP,
                      nullptr, nullptr, &si, &pi)) {
    throw std::runtime_error("failed to spawn firmiusd");
  }
  if (!options_.spawnedDaemonPidFile.empty()) {
    std::ofstream pidOut(options_.spawnedDaemonPidFile, std::ios::trunc);
    pidOut << static_cast<unsigned long>(pi.dwProcessId);
  }
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
#else
  pid_t pid = -1;
  std::string endpoint = options_.connection.endpoint;
  char *argv[] = {const_cast<char *>(command.c_str()), const_cast<char *>("--endpoint"),
                  endpoint.data(), nullptr};
  const int rc = command.find('/') == std::string::npos
      ? posix_spawnp(&pid, command.c_str(), nullptr, nullptr, argv, environ)
      : posix_spawn(&pid, command.c_str(), nullptr, nullptr, argv, environ);
  if (rc != 0) {
    throw std::runtime_error("failed to spawn firmiusd");
  }
  if (!options_.spawnedDaemonPidFile.empty()) {
    std::ofstream pidOut(options_.spawnedDaemonPidFile, std::ios::trunc);
    pidOut << pid;
  }
#endif
}

firmius::core::JsonRpcTransport &DaemonClient::transport() const {
  if (!transport_) {
    firmius::daemon::SocketTransport socketTransport(options_.connection);
    auto channel = socketTransport.connect(std::chrono::milliseconds(300));
    transport_ = std::make_unique<firmius::core::JsonRpcTransport>(
        std::move(channel.writer), std::move(channel.reader), std::move(channel.wakeStop));
    transport_->start();
  }
  return *transport_;
}

} // namespace firmius::daemon

#ifndef FIRMIUS_SERVER_DAEMONCLIENT_HPP
#define FIRMIUS_SERVER_DAEMONCLIENT_HPP

#include "daemon/Protocol.hpp"
#include "lsp/JsonRpcTransport.hpp"

#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace firmius::daemon {

class DaemonClient {
public:
  using DaemonEventListener = std::function<void(const DaemonEventEnvelope &)>;

  explicit DaemonClient(DaemonClientOptions options = {});
  ~DaemonClient();

  bool connect();
  void disconnect();
  bool connected() const;

  UiSnapshot uiSnapshot(const UiSnapshotRequest &request = {}) const;
  DaemonPingResponse ping() const;
  ClientSessionSnapshot session() const;
  std::vector<ClientSessionSnapshot> listClients() const;

  std::vector<firmius::shared::ThreadMetadata> listThreads() const;
  std::vector<ThreadOverview>
  listThreadOverviews(const ThreadOverviewRequest &request) const;
  ThreadSnapshot getThread(const ThreadsOpenRequest &request) const;
  ThreadsOpenResponse openThread(const std::string &threadId) const;
  ThreadsOpenResponse openThread(const ThreadsOpenRequest &request) const;
  ThreadSnapshot focusThread(const ThreadsOpenRequest &request) const;
  ThreadsCreateResponse createThread(const ThreadsCreateRequest &request) const;
  ThreadsSendResponse send(const ThreadsSendRequest &request) const;

  AgentTreeSnapshot listAgents(const AgentTargetRequest &request) const;
  std::optional<AgentRuntimeSnapshot>
  getAgent(const AgentTargetRequest &request) const;
  std::optional<AgentTodoSnapshot>
  getAgentTodo(const AgentTargetRequest &request) const;
  std::optional<AgentRuntimeSnapshot>
  focusAgent(const AgentTargetRequest &request) const;
  bool compactAgent(const AgentTargetRequest &request) const;
  bool interruptAgent(const AgentTargetRequest &request) const;
  bool abortAndFlushQueuedMessages(const AgentTargetRequest &request) const;
  std::optional<AgentRuntimeSnapshot>
  setAgentMode(const AgentsSetModeRequest &request) const;

  std::vector<ProcessSnapshot>
  listProcesses(const ProcessesListRequest &request) const;
  std::optional<ProcessSnapshot>
  getProcess(const ProcessesGetRequest &request) const;
  ProcessRuntimeSummary focusProcessState(const ProcessesListRequest &request) const;

  std::optional<TranscriptSnapshot>
  getTranscript(const TranscriptGetRequest &request) const;
  std::vector<ToolCallSnapshot>
  listToolCalls(const ToolCallsListRequest &request) const;
  SubagentActivitySnapshot
  subagentActivity(const SubagentsActivityRequest &request) const;
  ArtifactCatalogSnapshot listArtifacts(const ThreadsOpenRequest &request) const;

  ModelCatalogSnapshot listModels(bool refresh = false) const;
  ModelCatalogSnapshot refreshModels() const;
  std::optional<AgentRuntimeSnapshot>
  switchModel(const ModelSwitchRequest &request) const;
  ProviderCatalogSnapshot listProviders() const;
  ProviderCatalogSnapshot
  updateProviderProfiles(const ProviderProfilesUpdateRequest &request) const;
  ModelCatalogSnapshot invalidateModelCache() const;
  std::vector<AccountSnapshot> listAccounts(const AccountsRequest &request) const;
  bool deleteAccount(const AccountDeleteRequest &request) const;
  QuotaSnapshot getQuotas(const QuotasRequest &request) const;
  QuotaSnapshot getCachedQuotas(const QuotasRequest &request) const;

  PermissionQueueSnapshot getPermissionMode(const PermissionModeRequest &request) const;
  PermissionQueueSnapshot setPermissionMode(const PermissionModeUpdateRequest &request) const;
  PermissionQueueSnapshot listPendingPermissions(const PermissionModeRequest &request) const;
  PermissionCreateModeResponse createPermissionMode(
      const PermissionCreateModeRequest &request) const;
  PermissionRenameModeResponse renamePermissionMode(
      const PermissionRenameModeRequest &request) const;
  PermissionDeleteModeResponse deletePermissionMode(
      const PermissionDeleteModeRequest &request) const;
  bool resolvePermission(const PermissionResolveRequest &request) const;
  bool resolvePermissionWithRules(
      const PermissionResolveWithRulesRequest &request) const;
  PermissionListRulesResponse listPolicyRules() const;
  PermissionUpsertRuleResponse upsertPolicyRule(
      const PermissionUpsertRuleRequest &request) const;
  PermissionDeleteRuleResponse deletePolicyRule(
      const PermissionDeleteRuleRequest &request) const;
  PermissionReloadPolicyResponse reloadPolicy() const;

  UserConfigSnapshot getConfig() const;
  UserConfigSnapshot updateConfig(const ConfigUpdateRequest &request) const;
  RouterConfigSnapshot getRouterConfig() const;
  RouterConfigSnapshot updateRouterConfig(const RouterConfigUpdateRequest &request) const;
  PurposesConfigSnapshot getPurposesConfig() const;
  PurposesConfigSnapshot
  updatePurposesConfig(const PurposesConfigUpdateRequest &request) const;
  McpConfigSnapshot getMcpConfig() const;
  McpConfigSnapshot updateMcpConfig(const McpConfigUpdateRequest &request) const;

  HistorySnapshot getHistory(const HistoryGetRequest &request) const;
  HistoryMutationResult undoHistory(const HistoryUndoRequest &request) const;
  HistoryMutationResult redoHistory(const HistoryRedoRequest &request) const;
  HistoryMutationResult undoTranscript(const HistoryUndoRequest &request) const;
  HistoryMutationResult redoTranscript(const HistoryRedoRequest &request) const;
  EditHistorySnapshot listEdits(const EditsListRequest &request) const;
  EditMutationResult undoEdit(const EditsUndoRequest &request) const;
  EditMutationResult redoEdit(const EditsRedoRequest &request) const;

  HookStatusSnapshot listHooks() const;
  HookStatusSnapshot reloadHooks() const;
  HookStateSnapshot hookState(const HooksStateRequest &request) const;
  HooksRecentActivitySnapshot
  recentHookActivity(const HooksRecentActivityRequest &request) const;
  std::vector<WorkflowExecutionSnapshot> listWorkflows() const;
  bool executeWorkflow(const WorkflowExecuteRequest &request) const;

  ModeCatalogSnapshot listModes() const;
  std::optional<ModeSnapshot> getMode(const ModesGetRequest &request) const;
  PersonaCatalogSnapshot listPersonas() const;
  ToolCatalogSnapshot toolCatalog() const;
  BenchmarkCatalogSnapshot listSupportedBenchmarks() const;
  BenchmarksStartResponse startBenchmark(const BenchmarksStartRequest &request) const;
  BenchmarkStatusSnapshot getBenchmarkStatus(const BenchmarksStatusRequest &request) const;
  BenchmarkLogsSnapshot getBenchmarkLogs(const BenchmarksLogsRequest &request) const;

  bool subscribe(DaemonEventListener listener,
                 const EventSubscriptionRequest &request = {});
  bool unsubscribe();

  // ── /connect wizard ──
  ConnectBeginResponse beginConnect(const ConnectBeginRequest &request) const;
  ConnectSubmitResponse submitConnect(const ConnectSubmitRequest &request) const;
  ConnectFinalizeResponse finalizeConnect(const ConnectFinalizeRequest &request) const;
  ConnectCancelResponse cancelConnect(const ConnectCancelRequest &request) const;

  // ── /undo Rewind ──
  RewindPreviewResponse previewRewind(const RewindPreviewRequest &request) const;
  RewindExecuteResponse executeRewind(const RewindExecuteRequest &request) const;
  RedoPreviewResponse previewRedo(const RedoPreviewRequest &request) const;
  RedoExecuteResponse executeRedo(const RedoExecuteRequest &request) const;

private:
  std::string daemonCommand() const;
  bool waitForConnectReady();
  void spawnDaemon() const;
  firmius::core::JsonRpcTransport &transport() const;

  DaemonClientOptions options_;
  mutable std::unique_ptr<firmius::core::JsonRpcTransport> transport_;
  bool connected_ = false;
  bool subscribed_ = false;
  ClientSessionSnapshot session_{};
  DaemonEventListener listener_;
};

} // namespace firmius::daemon

#endif // FIRMIUS_SERVER_DAEMONCLIENT_HPP

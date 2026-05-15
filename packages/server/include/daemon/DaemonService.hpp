#pragma once

#include "daemon/Protocol.hpp"

#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <unordered_set>
#include <string>
#include <unordered_map>
#include <vector>

namespace firmius::daemon {

class DaemonService {
public:
  using DaemonEventListener = std::function<void(const DaemonEventEnvelope &)>;

  DaemonService();
  ~DaemonService();

  void start();
  void shutdown();
  bool running() const;

  DaemonPingResponse ping() const;
  DaemonAuditEmitRuntimeEventResponse
  auditEmitRuntimeEvent(const DaemonAuditEmitRuntimeEventRequest &request);
  ClientHelloResponse registerClient(const ClientHelloRequest &request);
  bool unregisterClient(const std::string &clientId);
  std::vector<ClientSessionSnapshot> listClients() const;
  std::vector<firmius::shared::ThreadMetadata> listThreads() const;
  ThreadSnapshot getThread(const std::string &clientId,
                           const ThreadsOpenRequest &request) const;
  ThreadsOpenResponse openThread(const std::string &clientId,
                                 const ThreadsOpenRequest &request);
  ThreadSnapshot focusThread(const std::string &clientId,
                             const ThreadsOpenRequest &request);
  ThreadsCreateResponse createThread(const std::string &clientId,
                                     const ThreadsCreateRequest &request);
  ThreadsSendResponse sendToThread(const std::string &clientId,
                                   const ThreadsSendRequest &request);
  AgentTreeSnapshot listAgents(const std::string &clientId,
                               const AgentTargetRequest &request) const;
  std::optional<AgentRuntimeSnapshot>
  getAgent(const std::string &clientId, const AgentTargetRequest &request) const;
  std::optional<AgentRuntimeSnapshot>
  focusAgent(const std::string &clientId, const AgentTargetRequest &request);
  std::optional<AgentRuntimeSnapshot>
  compactAgent(const AgentTargetRequest &request);
  std::optional<AgentRuntimeSnapshot>
  interruptAgent(const AgentTargetRequest &request);
  std::vector<ProcessSnapshot> listProcesses(const ProcessesListRequest &request) const;
  std::optional<ProcessSnapshot> getProcess(const ProcessesGetRequest &request) const;
  ProcessRuntimeSummary focusProcessState(const std::string &clientId,
                                          const ProcessesListRequest &request) const;
  std::optional<TranscriptSnapshot>
  getTranscript(const std::string &clientId,
                const TranscriptGetRequest &request) const;
  std::vector<ToolCallSnapshot>
  listToolCalls(const std::string &clientId,
                const ToolCallsListRequest &request) const;
  SubagentActivitySnapshot
  subagentActivity(const std::string &clientId,
                   const SubagentsActivityRequest &request) const;
  PermissionQueueSnapshot
  getPermissionQueue(const std::string &clientId,
                     const PermissionModeRequest &request) const;
  PermissionQueueSnapshot
  setPermissionMode(const std::string &clientId,
                    const PermissionModeUpdateRequest &request);
  bool resolvePermission(const PermissionResolveRequest &request);
  ModelCatalogSnapshot listModels(bool refresh);
  std::optional<AgentRuntimeSnapshot> switchModel(const ModelSwitchRequest &request);
  ProviderCatalogSnapshot listProviders() const;
  ProviderCatalogSnapshot
  updateProviderProfiles(const ProviderProfilesUpdateRequest &request);
  ModelCatalogSnapshot invalidateModelCache();
  HistorySnapshot getHistory(const std::string &clientId,
                             const HistoryGetRequest &request) const;
  HistoryMutationResult
  undoHistory(const std::string &clientId,
              const HistoryUndoRequest &request) const;
  HistoryMutationResult
  redoHistory(const std::string &clientId,
              const HistoryRedoRequest &request) const;
  EditHistorySnapshot
  listEdits(const std::string &clientId, const EditsListRequest &request) const;
  EditMutationResult
  undoEdit(const std::string &clientId, const EditsUndoRequest &request) const;
  EditMutationResult
  redoEdit(const std::string &clientId, const EditsRedoRequest &request) const;
  std::vector<AccountSnapshot> listAccounts(const AccountsRequest &request) const;
  bool deleteAccount(const AccountDeleteRequest &request);
  QuotaSnapshot getQuotas(const QuotasRequest &request, bool refresh) const;
  UserConfigSnapshot getConfig() const;
  UserConfigSnapshot updateConfig(const ConfigUpdateRequest &request);
  RouterConfigSnapshot getRouterConfig() const;
  RouterConfigSnapshot updateRouterConfig(const RouterConfigUpdateRequest &request);
  PurposesConfigSnapshot getPurposesConfig() const;
  PurposesConfigSnapshot
  updatePurposesConfig(const PurposesConfigUpdateRequest &request);
  RollingMemoryConfigSnapshot getRollingMemoryConfig() const;
  RollingMemoryConfigSnapshot
  updateRollingMemoryConfig(const RollingMemoryConfigUpdateRequest &request);
  McpConfigSnapshot getMcpConfig() const;
  McpConfigSnapshot updateMcpConfig(const McpConfigUpdateRequest &request);
  HookStatusSnapshot listHooks() const;
  HookStatusSnapshot reloadHooks();
  HookStateSnapshot hookState(const HooksStateRequest &request) const;
  std::vector<PactSnapshot> listPacts(const std::string &clientId,
                                      const PactsListRequest &request) const;
  std::optional<PactSnapshot> getPact(const std::string &clientId,
                                      const PactsGetRequest &request) const;
  std::vector<WorkflowExecutionSnapshot> listWorkflows() const;
  bool executeWorkflow(const WorkflowExecuteRequest &request);
  ArtifactCatalogSnapshot
  listArtifacts(const std::string &clientId, const ThreadsOpenRequest &request) const;
  EventSubscriptionResponse subscribe(const std::string &clientId,
                                      const EventSubscriptionRequest &request,
                                      DaemonEventListener listener);
  EventSubscriptionResponse unsubscribe(const std::string &clientId);

  ModeCatalogSnapshot listModes() const;
  std::optional<ModeSnapshot> getMode(const ModesGetRequest& request) const;
  std::optional<AgentRuntimeSnapshot> setAgentMode(const std::string& clientId, const AgentsSetModeRequest& request);
  PersonaCatalogSnapshot listPersonas() const;
  ToolCatalogSnapshot toolCatalog() const;
  BenchmarkCatalogSnapshot listSupportedBenchmarks() const;
  HooksRecentActivitySnapshot recentHookActivity(const HooksRecentActivityRequest& request) const;

  std::optional<ClientSessionSnapshot> session(const std::string &clientId) const;
  UiSnapshot uiSnapshot(const std::string &clientId,
                        const UiSnapshotRequest &request);
  BenchmarksStartResponse startBenchmark(const std::string &clientId,
                                        const BenchmarksStartRequest &request);
  BenchmarkStatusSnapshot getBenchmarkStatus(const std::string &clientId,
                                             const BenchmarksStatusRequest &request) const;
  BenchmarkLogsSnapshot getBenchmarkLogs(const std::string &clientId,
                                         const BenchmarksLogsRequest &request) const;

private:
  struct EventSubscription {
    std::unordered_set<std::string> eventKinds;
    std::uint64_t sinceSequence = 0;
    DaemonEventListener listener;
  };

  DaemonEventEnvelope prepareEventEnvelope(DaemonEventEnvelope envelope);
  void storeEventEnvelopeLocked(const DaemonEventEnvelope &envelope);

  void emitSessionEvent(DaemonEventKind kind, const ClientSessionSnapshot &session);
  void emitHookStateEvent(const HookStateSnapshot &snapshot);
  void emitPactStateEvent(const PactSnapshot &snapshot);
  void emitCoreEvent(const firmius::shared::AppEvent &event);
  std::uint64_t nowMs() const;
  void emitRuntimeEventToFocusedClients(const std::string &eventType,
                                        const std::string &eventThreadId,
                                        const std::string &eventAgentId,
                                        const std::string &eventJson);
  void updateSessionFocusLocked(const std::string &clientId,
                                const std::string &threadId,
                                const std::string &agentId);
  std::string resolveThreadIdForRequest(const std::string &clientId,
                                        const std::string &requestedThreadId) const;
  std::string resolveAgentIdForRequest(const std::string &clientId,
                                       const std::string &requestedThreadId,
                                       const std::string &requestedAgentId) const;
  ThreadSnapshot buildThreadSnapshotLocked(const std::string &threadId,
                                           const std::string &focusedThreadId,
                                           const std::string &focusedAgentId) const;
  std::vector<AgentRuntimeSnapshot>
  buildAgentSnapshotsLocked(const std::string &threadId,
                            const std::string &focusedAgentId) const;
  std::optional<AgentRuntimeSnapshot>
  buildAgentSnapshotLocked(const std::string &threadId,
                           const std::string &agentId,
                           const std::string &focusedAgentId) const;
  std::vector<ProcessSnapshot>
  buildProcessSnapshotsLocked(const std::string &threadId,
                              const std::string &agentId) const;
  std::optional<ProcessSnapshot>
  buildProcessSnapshotLocked(const std::string &threadId,
                             const std::string &agentId,
                             const std::string &processId) const;
  std::optional<TranscriptSnapshot>
  buildTranscriptSnapshotLocked(const std::string &threadId,
                                const std::string &agentId) const;
  std::vector<ToolCallSnapshot>
  buildToolCallSnapshotsLocked(const std::string &threadId,
                               const std::string &agentId) const;
  SubagentActivitySnapshot
  buildSubagentActivitySnapshotLocked(const std::string &threadId,
                                      const std::string &agentId) const;
  HistorySnapshot buildHistorySnapshotLocked(const std::string &threadId,
                                             const std::string &agentId,
                                             int limit) const;
  EditHistorySnapshot buildEditHistorySnapshotLocked(
      const std::string &threadId, const std::string &agentId,
      bool includeUndone) const;
  HookStateSnapshot buildHookStateSnapshotLocked(const HooksStateRequest &request) const;
  std::vector<PactSnapshot> buildPactSnapshotsLocked(const std::string &threadId,
                                                     const std::string &agentId) const;
  std::string hookStateChangeKey(const HookStateSnapshot &snapshot) const;
  std::string pactStateChangeKey(const PactSnapshot &snapshot) const;

  mutable std::mutex stateMutex_;
  mutable std::mutex runtimeMutex_;
  bool running_ = false;
  int harnessSubscriptionId_ = -1;
  std::unordered_map<std::string, ClientSessionSnapshot> sessions_;
  std::unordered_map<std::string, EventSubscription> subscriptions_;
  std::uint64_t nextEventSequence_ = 1;
  std::vector<DaemonEventEnvelope> eventReplayBuffer_;
  static constexpr std::size_t kMaxReplayEvents = 2048;

  std::unordered_map<std::string, std::string> hookStateChangeKeys_;
  std::unordered_map<std::string, std::string> pactStateChangeKeys_;
};

} // namespace firmius::daemon

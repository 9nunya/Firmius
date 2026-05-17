#pragma once

#include "Context.hpp"
#include "ConfigLoader.hpp"
#include "Events.hpp"
#include "Enums.hpp"

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace firmius::daemon {

inline constexpr const char *kProtocolVersion = "1";
inline constexpr const char *kRpcUiSnapshotGet = "ui.snapshot.get";
inline constexpr const char *kRpcDaemonPing = "daemon.ping";
inline constexpr const char *kRpcDaemonAuditEmitRuntimeEvent =
    "daemon.auditEmitRuntimeEvent";
inline constexpr const char *kRpcClientHello = "client.hello";
inline constexpr const char *kRpcClientGoodbye = "client.goodbye";
inline constexpr const char *kRpcThreadsList = "threads.list";
inline constexpr const char *kRpcThreadsGet = "threads.get";
inline constexpr const char *kRpcThreadsOpen = "threads.open";
inline constexpr const char *kRpcThreadsFocus = "threads.focus";
inline constexpr const char *kRpcThreadsCreate = "threads.create";
inline constexpr const char *kRpcThreadsSend = "threads.send";
inline constexpr const char *kRpcAgentsList = "agents.list";
inline constexpr const char *kRpcAgentsGet = "agents.get";
inline constexpr const char *kRpcAgentsFocus = "agents.focus";
inline constexpr const char *kRpcAgentTodoGet = "agentTodo.get";
inline constexpr const char *kRpcAgentsCompact = "agents.compact";
inline constexpr const char *kRpcAgentsInterrupt = "agents.interrupt";
inline constexpr const char *kRpcProcessesList = "processes.list";
inline constexpr const char *kRpcProcessesGet = "processes.get";
inline constexpr const char *kRpcProcessesFocusState = "processes.focusState";
inline constexpr const char *kRpcTranscriptGet = "transcript.get";
inline constexpr const char *kRpcToolCallsList = "toolcalls.list";
inline constexpr const char *kRpcSubagentsActivity = "subagents.activity";
inline constexpr const char *kRpcModelsList = "models.list";
inline constexpr const char *kRpcModelsRefresh = "models.refresh";
inline constexpr const char *kRpcModelsSwitch = "models.switch";
inline constexpr const char *kRpcProvidersList = "providers.list";
inline constexpr const char *kRpcProvidersUpdateProfiles =
    "providers.updateProfiles";
inline constexpr const char *kRpcProvidersInvalidateModelCache =
    "providers.invalidateModelCache";
inline constexpr const char *kRpcAccountsList = "accounts.list";
inline constexpr const char *kRpcAccountsDelete = "accounts.delete";
inline constexpr const char *kRpcQuotasGet = "quotas.get";
inline constexpr const char *kRpcQuotasGetCached = "quotas.getCached";
inline constexpr const char *kRpcPermissionsGetMode = "permissions.getMode";
inline constexpr const char *kRpcPermissionsSetMode = "permissions.setMode";
inline constexpr const char *kRpcPermissionsListPending =
    "permissions.listPending";
inline constexpr const char *kRpcPermissionsResolve = "permissions.resolve";
inline constexpr const char *kRpcConfigGet = "config.get";
inline constexpr const char *kRpcConfigUpdate = "config.update";
inline constexpr const char *kRpcHistoryGet = "history.get";
inline constexpr const char *kRpcHistoryUndo = "history.undo";
inline constexpr const char *kRpcHistoryRedo = "history.redo";
inline constexpr const char *kRpcHistoryUndoTranscript = "history.undoTranscript";
inline constexpr const char *kRpcHistoryRedoTranscript = "history.redoTranscript";
inline constexpr const char *kRpcRouterGet = "router.get";
inline constexpr const char *kRpcRouterUpdate = "router.update";
inline constexpr const char *kRpcPurposesGet = "purposes.get";
inline constexpr const char *kRpcPurposesUpdate = "purposes.update";
inline constexpr const char *kRpcRollingMemoryGet = "rollingMemory.get";
inline constexpr const char *kRpcRollingMemoryUpdate = "rollingMemory.update";
inline constexpr const char *kRpcMcpGet = "mcp.get";
inline constexpr const char *kRpcMcpUpdate = "mcp.update";
inline constexpr const char *kRpcHooksList = "hooks.list";
inline constexpr const char *kRpcHooksReload = "hooks.reload";
inline constexpr const char *kRpcHooksState = "hooks.state";
inline constexpr const char *kRpcPactsList = "pacts.list";
inline constexpr const char *kRpcPactsGet = "pacts.get";
inline constexpr const char *kRpcWorkflowsList = "workflows.list";
inline constexpr const char *kRpcWorkflowsExecute = "workflows.execute";
inline constexpr const char *kRpcEditsList = "edits.list";
inline constexpr const char *kRpcEditsUndo = "edits.undo";
inline constexpr const char *kRpcEditsRedo = "edits.redo";
inline constexpr const char *kRpcArtifactsList = "artifacts.list";
inline constexpr const char *kRpcEventsSubscribe = "events.subscribe";
inline constexpr const char *kRpcEventsUnsubscribe = "events.unsubscribe";
inline constexpr const char *kRpcClientsList = "clients.list";
inline constexpr const char *kRpcBenchmarksStart = "benchmarks.start";
inline constexpr const char *kRpcBenchmarksStatus = "benchmarks.status";
inline constexpr const char *kRpcBenchmarksLogs = "benchmarks.logs";
inline constexpr const char *kRpcModesList = "modes.list";
inline constexpr const char *kRpcModesGet = "modes.get";
inline constexpr const char *kRpcAgentsSetMode = "agents.setMode";
inline constexpr const char *kRpcPersonasList = "personas.list";
inline constexpr const char *kRpcToolsCatalog = "tools.catalog";
inline constexpr const char *kRpcBenchmarksListSupported = "benchmarks.listSupported";
inline constexpr const char *kRpcHooksRecentActivity = "hooks.recentActivity";
inline constexpr const char *kNotificationDaemonEvent = "daemon.event";

#if defined(_WIN32)
inline constexpr const char *kDefaultEndpoint = R"(\\.\pipe\firmiusd)";
#else
inline constexpr const char *kDefaultEndpoint = "/tmp/firmiusd.sock";
#endif

enum class DaemonEventKind {
  RuntimeAppEvent,
  ClientSessionRegistered,
  ClientSessionDisconnected,
  ClientSessionUpdated,
  HookStateChanged,
  PactStateChanged,
  InitProgress,
};

struct WorkspacePresence {
  std::string cwd;
  std::string workspaceRoot;
  std::string repoRoot;

  bool operator==(const WorkspacePresence &) const = default;
};

struct ClientIdentity {
  std::string clientId;
  std::string uiKind;
  int pid = 0;
  std::vector<std::string> capabilityFlags;

  bool operator==(const ClientIdentity &) const = default;
};

struct ClientSessionSnapshot {
  ClientIdentity identity;
  WorkspacePresence presence;
  std::string focusedThreadId;
  std::string focusedAgentId;
  std::uint64_t connectedAtMs = 0;
  std::uint64_t lastSeenAtMs = 0;
  bool subscribed = false;

  bool operator==(const ClientSessionSnapshot &) const = default;
};

struct DaemonConnectionInfo {
  std::string endpoint = kDefaultEndpoint;

  bool operator==(const DaemonConnectionInfo &) const = default;
};

struct DaemonClientOptions {
  ClientIdentity identity;
  WorkspacePresence presence;
  DaemonConnectionInfo connection;
  std::string daemonExecutablePath;
  std::string spawnedDaemonPidFile;
  bool autoStart = true;
  bool subscribeToEvents = true;

  bool operator==(const DaemonClientOptions &) const = default;
};

struct DaemonPingResponse {
  std::string protocolVersion = kProtocolVersion;
  std::string serverName = "firmiusd";
  int pid = 0;
  bool ok = true;

  bool operator==(const DaemonPingResponse &) const = default;
};

struct DaemonAuditEmitRuntimeEventRequest {
  std::string eventType;
  std::string threadId;
  std::string agentId;
  std::string parentAgentId;
  std::string text;
  std::string toolCallId;
  std::string toolName;
  std::string toolArgsJson;

  bool operator==(const DaemonAuditEmitRuntimeEventRequest &) const = default;
};

struct DaemonAuditEmitRuntimeEventResponse {
  bool emitted = false;
  std::string runtimeEventType;
  std::string threadId;
  std::string agentId;

  bool operator==(const DaemonAuditEmitRuntimeEventResponse &) const = default;
};

struct ClientHelloRequest {
  std::string protocolVersion = kProtocolVersion;
  ClientIdentity identity;
  WorkspacePresence presence;
  std::string focusedThreadId;
  std::string focusedAgentId;

  bool operator==(const ClientHelloRequest &) const = default;
};

struct ClientHelloResponse {
  std::string protocolVersion = kProtocolVersion;
  ClientSessionSnapshot session;

  bool operator==(const ClientHelloResponse &) const = default;
};

struct ClientGoodbyeRequest {
  std::string clientId;

  bool operator==(const ClientGoodbyeRequest &) const = default;
};

struct ThreadsOpenRequest {
  std::string threadId;

  bool operator==(const ThreadsOpenRequest &) const = default;
};

struct AgentTargetRequest {
  std::string threadId;
  std::string agentId;

  bool operator==(const AgentTargetRequest &) const = default;
};

struct ProcessesListRequest {
  std::string threadId;
  std::string agentId;

  bool operator==(const ProcessesListRequest &) const = default;
};

struct ProcessesGetRequest {
  std::string threadId;
  std::string agentId;
  std::string processId;

  bool operator==(const ProcessesGetRequest &) const = default;
};

struct AccountsRequest {
  std::string providerId;

  bool operator==(const AccountsRequest &) const = default;
};

struct AccountDeleteRequest {
  std::string providerId;
  std::string identifier;

  bool operator==(const AccountDeleteRequest &) const = default;
};

struct QuotasRequest {
  std::string providerId;

  bool operator==(const QuotasRequest &) const = default;
};

struct PermissionModeRequest {
  std::string threadId;

  bool operator==(const PermissionModeRequest &) const = default;
};

struct PermissionModeUpdateRequest {
  std::string threadId;
  firmius::shared::ThreadPermissionMode permissionMode =
      firmius::shared::ThreadPermissionMode::Request;

  bool operator==(const PermissionModeUpdateRequest &) const = default;
};

struct PermissionResolveRequest {
  std::string requestId;
  firmius::shared::PermissionResponse response =
      firmius::shared::PermissionResponse::Deny;

  bool operator==(const PermissionResolveRequest &) const = default;
};

struct ModelSwitchRequest {
  std::string agentId;
  std::string providerId;
  std::string modelId;
  std::string variantName;

  bool operator==(const ModelSwitchRequest &) const = default;
};

struct ProviderProfilesUpdateRequest {
  std::map<std::string, firmius::shared::ProviderProfileConfig> providers;

  bool operator==(const ProviderProfilesUpdateRequest &) const = default;
};

struct ConfigUpdateRequest {
  firmius::shared::UserConfig config;

  bool operator==(const ConfigUpdateRequest &) const = default;
};

struct RouterConfigUpdateRequest {
  std::map<std::string, firmius::shared::ModelRouteCategory> categories;
  std::string defaultRouteCategory;
  bool enableSubagentRouteFallback = true;
  std::vector<std::string> subagentRouteFallbackOrder;

  bool operator==(const RouterConfigUpdateRequest &) const = default;
};

struct PurposesConfigUpdateRequest {
  std::map<std::string, std::string> purposeRoutes;

  bool operator==(const PurposesConfigUpdateRequest &) const = default;
};

struct RollingMemoryConfigUpdateRequest {
  firmius::shared::UserConfig::RollingMemoryConfig rollingMemory;

  bool operator==(const RollingMemoryConfigUpdateRequest &) const = default;
};

struct McpConfigUpdateRequest {
  std::map<std::string, firmius::shared::McpServerConfig> servers;

  bool operator==(const McpConfigUpdateRequest &) const = default;
};

struct ModesListRequest {
  bool operator==(const ModesListRequest &) const = default;
};

struct ModesGetRequest {
  std::string modeId;
  bool operator==(const ModesGetRequest &) const = default;
};

struct AgentsSetModeRequest {
  std::string threadId;
  std::string agentId;
  std::string modeId;
  bool operator==(const AgentsSetModeRequest &) const = default;
};

struct PersonasListRequest {
  bool operator==(const PersonasListRequest &) const = default;
};

struct ToolsCatalogRequest {
  bool operator==(const ToolsCatalogRequest &) const = default;
};

struct BenchmarksListSupportedRequest {
  bool operator==(const BenchmarksListSupportedRequest &) const = default;
};

struct HooksRecentActivityRequest {
  std::string threadId;
  bool operator==(const HooksRecentActivityRequest &) const = default;
};

struct HooksStateRequest {
  std::string threadId;
  std::string agentId;
  std::string hookId;
  int limit = 24;

  bool operator==(const HooksStateRequest &) const = default;
};

struct PactsListRequest {
  std::string threadId;
  std::string agentId;

  bool operator==(const PactsListRequest &) const = default;
};

struct PactsGetRequest {
  std::string threadId;
  std::string pactId;

  bool operator==(const PactsGetRequest &) const = default;
};

struct WorkflowExecuteRequest {
  std::string workflowId;
  std::vector<std::string> args;

  bool operator==(const WorkflowExecuteRequest &) const = default;
};

struct HistoryGetRequest {
  std::string threadId;
  std::string agentId;
  int limit = 20;

  bool operator==(const HistoryGetRequest &) const = default;
};

struct HistoryUndoRequest {
  std::string threadId;
  std::string agentId;
  int count = 1;

  bool operator==(const HistoryUndoRequest &) const = default;
};

struct HistoryRedoRequest {
  std::string threadId;
  std::string agentId;
  std::string undoActionId;

  bool operator==(const HistoryRedoRequest &) const = default;
};

struct EditsListRequest {
  std::string threadId;
  std::string agentId;
  bool includeUndone = true;

  bool operator==(const EditsListRequest &) const = default;
};

struct EditsUndoRequest {
  std::string threadId;
  std::string agentId;
  std::string editBatchId;

  bool operator==(const EditsUndoRequest &) const = default;
};

struct EditsRedoRequest {
  std::string threadId;
  std::string agentId;
  std::string undoActionId;

  bool operator==(const EditsRedoRequest &) const = default;
};

struct TranscriptGetRequest {
  std::string threadId;
  std::string agentId;

  bool operator==(const TranscriptGetRequest &) const = default;
};

struct ToolCallsListRequest {
  std::string threadId;
  std::string agentId;

  bool operator==(const ToolCallsListRequest &) const = default;
};

struct SubagentsActivityRequest {
  std::string threadId;
  std::string agentId;

  bool operator==(const SubagentsActivityRequest &) const = default;
};

struct BenchmarksStartRequest {
  std::string benchmarkId;
  std::string taskId;
  firmius::shared::HostCreationOptions hostOptions;
  std::string cwd = "/work";
  std::string personaName = "forge";

  bool operator==(const BenchmarksStartRequest &) const = default;
};

struct BenchmarksStartResponse {
  bool started = false;
  std::string threadId;
  std::string agentId;
  std::string benchmarkId;
  std::string taskId;

  bool operator==(const BenchmarksStartResponse &) const = default;
};

struct BenchmarksStatusRequest {
  std::string threadId;
  std::string agentId;

  bool operator==(const BenchmarksStatusRequest &) const = default;
};

struct BenchmarkStatusSnapshot {
  std::string threadId;
  std::string agentId;
  bool isBenchmarkRun = false;
  std::string benchmarkId;
  std::string taskId;
  bool agentLive = false;

  bool operator==(const BenchmarkStatusSnapshot &) const = default;
};

struct BenchmarksLogsRequest {
  std::string threadId;
  std::string agentId;
  int limit = 200;

  bool operator==(const BenchmarksLogsRequest &) const = default;
};

struct BenchmarkLogsSnapshot {
  std::string threadId;
  std::string agentId;
  std::vector<std::string> lines;

  bool operator==(const BenchmarkLogsSnapshot &) const = default;
};

struct ThreadsOpenResponse {
  bool opened = false;
  firmius::shared::ThreadMetadata thread;
  std::string focusedAgentId;

  bool operator==(const ThreadsOpenResponse &) const = default;
};

struct ThreadsCreateRequest {
  std::string cwd;
  std::string leadPersona;
  std::string initialMode;
  firmius::shared::ThreadPermissionMode permissionMode =
      firmius::shared::ThreadPermissionMode::Request;

  bool operator==(const ThreadsCreateRequest &) const = default;
};

struct ThreadsCreateResponse {
  firmius::shared::ThreadMetadata thread;
  std::string focusedAgentId;

  bool operator==(const ThreadsCreateResponse &) const = default;
};

struct ThreadsSendRequest {
  std::string threadId;
  std::string agentId;
  std::string text;
  std::vector<firmius::shared::ImageContent> images;

  bool operator==(const ThreadsSendRequest &) const = default;
};

struct ThreadsSendResponse {
  bool accepted = false;
  std::string threadId;
  std::string focusedAgentId;

  bool operator==(const ThreadsSendResponse &) const = default;
};

struct ThreadSnapshot {
  firmius::shared::ThreadMetadata thread;
  std::string focusedAgentId;
  std::vector<std::string> agentIds;
  std::size_t artifactCount = 0;
  std::size_t pendingPermissionCount = 0;
  bool focused = false;

  bool operator==(const ThreadSnapshot &) const = default;
};

struct AgentRuntimeSnapshot {
  std::string threadId;
  std::string agentId;
  std::string parentAgentId;
  std::string persona;
  std::string friendlyName;
  std::string title;
  std::string cwd;
  std::string hostId;
  std::string activeMode;
  firmius::shared::AgentStatus status = firmius::shared::AgentStatus::Idle;
  std::string providerId;
  std::string modelId;
  std::string variantName;
  uint32_t maxTokens = 0;
  uint32_t contextWindowTokens = 0;
  uint32_t contextUsedTokens = 0;
  uint32_t contextSentTokens = 0;
  std::vector<std::string> pendingToolCalls;
  std::vector<std::string> ownedProcesses;
  std::vector<std::string> blockingProcessIds;
  std::optional<std::string> fatalError;
  bool running = false;
  bool booting = false;
  bool focused = false;
  bool live = false;

  bool operator==(const AgentRuntimeSnapshot &) const = default;
};

struct AgentTodoSnapshot {
  std::string threadId;
  std::string agentId;
  int nextId = 1;
  std::vector<firmius::shared::TodoItem> items;

  bool operator==(const AgentTodoSnapshot &) const = default;
};

struct AgentTreeSnapshot {
  std::string threadId;
  std::string focusedAgentId;
  std::vector<AgentRuntimeSnapshot> agents;

  bool operator==(const AgentTreeSnapshot &) const = default;
};

struct ModeSnapshot {
  std::string modeId;
  std::string name;
  std::string description;
  bool operator==(const ModeSnapshot&) const = default;
};

struct ModeCatalogSnapshot {
  std::vector<ModeSnapshot> modes;
  bool operator==(const ModeCatalogSnapshot&) const = default;
};

struct PersonaSnapshot {
  std::string id;
  std::string name;
  std::string title;
  std::string description;
  std::vector<firmius::shared::ToolScope> allowedScopes;
  bool operator==(const PersonaSnapshot&) const = default;
};

struct PersonaCatalogSnapshot {
  std::vector<PersonaSnapshot> personas;
  bool operator==(const PersonaCatalogSnapshot&) const = default;
};

struct ToolSnapshot {
  std::string name;
  std::string description;
  std::vector<firmius::shared::ToolScope> scopes;
  bool operator==(const ToolSnapshot&) const = default;
};

struct ToolCatalogSnapshot {
  std::vector<ToolSnapshot> tools;
  bool operator==(const ToolCatalogSnapshot&) const = default;
};

struct BenchmarkCatalogSnapshot {
  std::vector<std::string> availableBenchmarks;
  bool operator==(const BenchmarkCatalogSnapshot&) const = default;
};

struct PermissionQueueSnapshot {
  std::string threadId;
  firmius::shared::ThreadPermissionMode permissionMode =
      firmius::shared::ThreadPermissionMode::Request;
  std::vector<firmius::shared::PermissionEscalationRequest> pending;

  bool operator==(const PermissionQueueSnapshot &) const = default;
};

struct ProcessSnapshot {
  std::string threadId;
  std::string agentId;
  std::string processId;
  std::string toolCallId;
  bool running = false;
  int exitCode = -1;
  std::string stdoutTail;
  std::string stderrTail;
  double durationMs = 0.0;
  std::string systemId;
  bool blocking = false;

  bool operator==(const ProcessSnapshot &) const = default;
};

struct ProcessRuntimeSummary {
  std::string threadId;
  std::string agentId;
  std::vector<std::string> activeProcessIds;
  std::vector<std::string> blockingProcessIds;
  std::size_t runningCount = 0;
  std::size_t blockingCount = 0;

  bool operator==(const ProcessRuntimeSummary &) const = default;
};

struct ModelCatalogSnapshot {
  std::vector<firmius::shared::ModelInfo> models;
  std::vector<std::string> fetchingProviders;
  bool loaded = false;
  bool loading = false;

  bool operator==(const ModelCatalogSnapshot &) const = default;
};

struct ProviderProfileSnapshot {
  std::string id;
  std::string kind;
  std::string authMode;
  std::string displayName;
  bool enabled = false;
  bool configured = false;
  bool custom = false;
  firmius::shared::ProviderProfileConfig profile;

  bool operator==(const ProviderProfileSnapshot &) const = default;
};

struct ProviderCatalogSnapshot {
  std::vector<ProviderProfileSnapshot> providers;

  bool operator==(const ProviderCatalogSnapshot &) const = default;
};

struct AccountSnapshot {
  std::string providerId;
  std::string identifier;
  bool rateLimited = false;
  std::int64_t backoffUntil = 0;
  std::map<std::string, std::string> metadata;

  bool operator==(const AccountSnapshot &) const = default;
};

struct QuotaSnapshot {
  std::string providerId;
  std::map<std::string, std::vector<firmius::shared::QuotaBucket>> buckets;

  bool operator==(const QuotaSnapshot &) const = default;
};

struct UserConfigSnapshot {
  firmius::shared::UserConfig config;

  bool operator==(const UserConfigSnapshot &) const = default;
};

struct RouterConfigSnapshot {
  std::map<std::string, firmius::shared::ModelRouteCategory> categories;
  std::string defaultRouteCategory;
  bool enableSubagentRouteFallback = true;
  std::vector<std::string> subagentRouteFallbackOrder;

  bool operator==(const RouterConfigSnapshot &) const = default;
};

struct PurposesConfigSnapshot {
  std::map<std::string, std::string> purposeRoutes;

  bool operator==(const PurposesConfigSnapshot &) const = default;
};

struct RollingMemoryConfigSnapshot {
  firmius::shared::UserConfig::RollingMemoryConfig rollingMemory;

  bool operator==(const RollingMemoryConfigSnapshot &) const = default;
};

struct McpConfigSnapshot {
  std::map<std::string, firmius::shared::McpServerConfig> servers;

  bool operator==(const McpConfigSnapshot &) const = default;
};

struct HookActivitySnapshot {
  std::string hookId;
  std::string threadId;
  std::string agentId;
  std::string eventName;
  std::string decision;
  std::string outcomeLabel;
  std::string blockReason;
  std::string statusLine;
  std::uint64_t timestampMs = 0;
  int stateWriteCount = 0;

  bool operator==(const HookActivitySnapshot &) const = default;
};

struct HooksRecentActivitySnapshot {
  std::vector<HookActivitySnapshot> activities;
  bool operator==(const HooksRecentActivitySnapshot &) const = default;
};

struct HookStatusSnapshot {
  std::vector<std::string> hookIds;
  std::vector<std::string> hookDirs;
  std::size_t hookCount = 0;

  bool operator==(const HookStatusSnapshot &) const = default;
};

struct HookStateSnapshot {
  std::string threadId;
  std::string agentId;
  std::string hookId;
  std::string snapshotJson;
  std::vector<HookActivitySnapshot> recentActivity;
  std::vector<std::string> currentStatusLines;
  std::vector<std::string> blockingReasons;
  std::string latestDecision;
  std::string latestOutcomeLabel;
  std::string latestStatusLine;
  std::uint64_t latestTimestampMs = 0;
  int totalStateWriteCount = 0;

  bool operator==(const HookStateSnapshot &) const = default;
};

struct PactHistoryEntrySnapshot {
  int iteration = 0;
  std::string validator;
  std::string validatorAgentId;
  std::string verdict;
  std::string suggestion;
  std::string evidenceJson;

  bool operator==(const PactHistoryEntrySnapshot &) const = default;
};

struct PactSnapshot {
  std::string threadId;
  std::string agentId;
  std::string pactId;
  std::string status;
  std::string title;
  std::string summary;
  std::string description;
  std::string validator;
  std::string lastVerdict;
  std::string lastSuggestion;
  std::string sealedBy;
  std::string statusLine;
  std::string blockingReason;
  std::string statePayloadJson;
  std::uint64_t createdAtMs = 0;
  std::uint64_t updatedAtMs = 0;
  int iteration = 0;
  int maxIterations = 0;
  bool active = false;
  bool resolved = false;
  bool failed = false;
  bool stale = false;
  std::vector<std::string> doneWhen;
  std::vector<PactHistoryEntrySnapshot> history;

  bool operator==(const PactSnapshot &) const = default;
};

struct WorkflowExecutionSnapshot {
  std::string workflowId;
  std::string name;
  std::string description;
  std::string slashCommand;
  bool hook = false;

  bool operator==(const WorkflowExecutionSnapshot &) const = default;
};

struct ArtifactCatalogSnapshot {
  std::string threadId;
  std::vector<firmius::shared::ThreadArtifactMetadata> artifacts;

  bool operator==(const ArtifactCatalogSnapshot &) const = default;
};

struct TranscriptSnapshot {
  std::string threadId;
  std::string agentId;
  std::string agentTitle;
  std::string agentFriendlyName;
  std::vector<firmius::shared::AgentTurn> rawTurns;
  std::vector<firmius::shared::AgentTurn> expandedTurns;

  bool operator==(const TranscriptSnapshot &) const = default;
};

struct ToolCallSnapshot {
  std::string threadId;
  std::string agentId;
  std::string toolCallId;
  std::string toolName;
  std::string toolArgsJson;
  std::string summary;
  std::string status;
  std::optional<bool> success;
  std::string resultJson;
  std::string resultSummary;
  std::string errorSummary;
  std::string processId;
  std::string subagentId;
  std::uint64_t issuedAtMs = 0;
  std::uint64_t completedAtMs = 0;

  bool operator==(const ToolCallSnapshot &) const = default;
};

struct SubagentActivityLogEntrySnapshot {
  std::string summary;
  std::string phase;
  std::string toolCallId;
  std::string toolName;
  std::string toolArgsJson;

  bool operator==(const SubagentActivityLogEntrySnapshot &) const = default;
};

struct SubagentActivityEntrySnapshot {
  std::string threadId;
  std::string parentAgentId;
  std::string parentToolCallId;
  std::string childAgentId;
  std::string childTitle;
  std::string childFriendlyName;
  std::string task;
  bool running = false;
  bool waiting = false;
  bool providerWaiting = false;
  bool retrying = false;
  bool accountSwitched = false;
  bool fallbackUsed = false;
  std::string waitState;
  std::string routeCategory;
  std::vector<std::string> attemptedCategories;
  std::string outcome;
  std::string finalSummary;
  std::string errorText;
  std::vector<std::string> artifactsCreated;
  std::vector<std::string> artifactsUpdated;
  std::vector<SubagentActivityLogEntrySnapshot> activityLog;

  bool operator==(const SubagentActivityEntrySnapshot &) const = default;
};

struct SubagentActivitySnapshot {
  std::string threadId;
  std::string agentId;
  std::vector<SubagentActivityEntrySnapshot> activities;

  bool operator==(const SubagentActivitySnapshot &) const = default;
};

struct HistorySnapshot {
  std::string threadId;
  std::string agentId;
  std::vector<firmius::shared::TranscriptUndoAction> recentUndoActions;
  std::vector<firmius::shared::TranscriptRedoEligibility> redoEligibilities;
  std::string latestRedoEligibleUndoActionId;

  bool operator==(const HistorySnapshot &) const = default;
};

struct HistoryMutationResult {
  bool applied = false;
  std::string threadId;
  std::string agentId;
  std::optional<firmius::shared::TranscriptUndoAction> undoAction;
  std::optional<firmius::shared::TranscriptRedoAction> redoAction;
  std::optional<firmius::shared::TranscriptRedoEligibility> redoEligibility;
  std::string message;
  HistorySnapshot history;

  bool operator==(const HistoryMutationResult &) const = default;
};

struct EditHistorySnapshot {
  std::string threadId;
  std::string agentId;
  std::vector<firmius::shared::EditBatchSummary> batches;
  std::vector<firmius::shared::EditUndoEligibility> undoEligibilities;

  bool operator==(const EditHistorySnapshot &) const = default;
};

struct EditMutationResult {
  bool applied = false;
  std::string threadId;
  std::string agentId;
  std::optional<firmius::shared::EditUndoAction> undoAction;
  std::optional<firmius::shared::EditRedoAction> redoAction;
  std::optional<firmius::shared::EditUndoEligibility> undoEligibility;
  std::optional<firmius::shared::EditRedoEligibility> redoEligibility;
  std::string message;
  EditHistorySnapshot edits;

  bool operator==(const EditMutationResult &) const = default;
};

struct UiSnapshotRequest {
  std::string threadId;
  std::string agentId;
  bool includeTranscript = true;
  bool includeToolCalls = true;
  bool includeProcesses = true;
  bool includeConfig = true;
  bool includeCatalogs = true;

  bool operator==(const UiSnapshotRequest &) const = default;
};

struct UiSnapshot {
  ClientSessionSnapshot session;
  std::vector<firmius::shared::ThreadMetadata> threads;
  std::optional<ThreadSnapshot> focusedThread;
  AgentTreeSnapshot agents;
  std::optional<AgentRuntimeSnapshot> focusedAgent;
  std::optional<AgentTodoSnapshot> focusedAgentTodo;
  std::optional<TranscriptSnapshot> transcript;
  std::vector<ToolCallSnapshot> toolCalls;
  SubagentActivitySnapshot subagents;
  ProcessRuntimeSummary processSummary;
  std::vector<ProcessSnapshot> processes;
  PermissionQueueSnapshot permissions;
  ModelCatalogSnapshot models;
  ProviderCatalogSnapshot providers;
  UserConfigSnapshot config;
  RouterConfigSnapshot router;
  PurposesConfigSnapshot purposes;
  RollingMemoryConfigSnapshot rollingMemory;
  McpConfigSnapshot mcp;
  HookStateSnapshot hooks;
  std::vector<PactSnapshot> pacts;
  ArtifactCatalogSnapshot artifacts;
  HistorySnapshot history;
  EditHistorySnapshot edits;
  std::uint64_t latestEventSequence = 0;

  bool operator==(const UiSnapshot &) const = default;
};

struct EventSubscriptionRequest {
  std::vector<std::string> eventKinds;
  std::uint64_t sinceSequence = 0;

  bool operator==(const EventSubscriptionRequest &) const = default;
};

struct EventSubscriptionResponse {
  bool subscribed = false;

  bool operator==(const EventSubscriptionResponse &) const = default;
};

struct DaemonEventEnvelope {
  DaemonEventKind kind = DaemonEventKind::RuntimeAppEvent;
  std::string subscriptionTarget;
  std::uint64_t serverTimestampMs = 0;
  std::uint64_t sequence = 0;
  std::optional<ClientSessionSnapshot> session;
  std::string runtimeEventType;
  std::string runtimeEventThreadId;
  std::string runtimeEventAgentId;
  std::string runtimeEventJson;
  std::optional<firmius::shared::AgentStatus> agentStatus;
  std::optional<HookStateSnapshot> hookState;
  std::optional<PactSnapshot> pactState;
  std::string initMessage;  // For InitProgress events: human-readable status

  bool operator==(const DaemonEventEnvelope &) const = default;
};

} // namespace firmius::daemon

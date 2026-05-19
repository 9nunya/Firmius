#ifndef FIRMIUS_SERVER_PROTOCOL_HPP
#define FIRMIUS_SERVER_PROTOCOL_HPP

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
inline constexpr const char *kRpcThreadsOverview = "threads.overview";
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
inline constexpr const char *kRpcAgentsAbortAndFlushQueuedMessages =
    "agents.abortAndFlushQueuedMessages";
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
inline constexpr const char *kRpcPermissionsCreateMode = "permissions.createMode";
inline constexpr const char *kRpcPermissionsRenameMode = "permissions.renameMode";
inline constexpr const char *kRpcPermissionsDeleteMode = "permissions.deleteMode";
inline constexpr const char *kRpcPermissionsListPending =
    "permissions.listPending";
inline constexpr const char *kRpcPermissionsResolve = "permissions.resolve";
inline constexpr const char *kRpcPermissionsResolveWithRules =
    "permissions.resolveWithRules";
inline constexpr const char *kRpcPermissionsListRules =
    "permissions.listRules";
inline constexpr const char *kRpcPermissionsUpsertRule =
    "permissions.upsertRule";
inline constexpr const char *kRpcPermissionsDeleteRule =
    "permissions.deleteRule";
inline constexpr const char *kRpcPermissionsReloadPolicy =
    "permissions.reloadPolicy";
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
inline constexpr const char *kRpcConnectBegin = "connect.begin";
inline constexpr const char *kRpcConnectSubmit = "connect.submit";
inline constexpr const char *kRpcConnectFinalize = "connect.finalize";
inline constexpr const char *kRpcConnectCancel = "connect.cancel";
inline constexpr const char *kRpcRewindPreview = "rewind.preview";
inline constexpr const char *kRpcRewindExecute = "rewind.execute";
inline constexpr const char *kRpcRedoPreview = "redo.preview";
inline constexpr const char *kRpcRedoExecute = "redo.execute";
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
  ConnectProgress,
  RewindApplied,
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
  // When set, the spawned daemon's stdout+stderr are redirected here. When
  // empty (the default), they are redirected to /dev/null on POSIX so the
  // daemon cannot scribble into the parent TUI's alt-screen. Stdin is always
  // redirected to /dev/null. Set FIRMIUS_DAEMON_LOG in the spawned env to
  // capture daemon log lines into a separate file regardless.
  std::string spawnedDaemonLogFile;
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

struct PermissionModeWire {
  std::string id;
  std::string name;
  std::string description;
  bool builtIn = false;

  bool operator==(const PermissionModeWire &) const = default;
};

struct PermissionModeRequest {
  bool operator==(const PermissionModeRequest &) const = default;
};

struct PermissionModeUpdateRequest {
  /// Mode id to switch to (e.g. "ask", "yolo", or a user-defined id).
  std::string modeId;

  bool operator==(const PermissionModeUpdateRequest &) const = default;
};

struct PermissionCreateModeRequest {
  /// Optional id; if empty, the engine generates one.
  std::string id;
  std::string name;
  std::string description;
  /// If true, copies the active mode's rules + category defaults as a
  /// starting point. Useful as "fork ask into custom-strict" UX.
  bool seedFromActive = false;

  bool operator==(const PermissionCreateModeRequest &) const = default;
};

struct PermissionCreateModeResponse {
  std::string modeId;          ///< Empty on collision/error.
  std::string errorMessage;

  bool operator==(const PermissionCreateModeResponse &) const = default;
};

struct PermissionRenameModeRequest {
  std::string modeId;
  std::string newName;

  bool operator==(const PermissionRenameModeRequest &) const = default;
};

struct PermissionRenameModeResponse {
  bool ok = false;
  std::string errorMessage;

  bool operator==(const PermissionRenameModeResponse &) const = default;
};

struct PermissionDeleteModeRequest {
  std::string modeId;

  bool operator==(const PermissionDeleteModeRequest &) const = default;
};

struct PermissionDeleteModeResponse {
  bool removed = false;
  std::string errorMessage;

  bool operator==(const PermissionDeleteModeResponse &) const = default;
};

struct PermissionResolveRequest {
  std::string requestId;
  firmius::shared::PermissionResponse response =
      firmius::shared::PermissionResponse::Deny;

  bool operator==(const PermissionResolveRequest &) const = default;
};

struct PermissionResolveWithRulesRequest {
  std::string requestId;
  /// `ruleId` values from the suggestion list the TUI received.
  std::vector<std::string> selectedSuggestionIds;

  bool operator==(const PermissionResolveWithRulesRequest &) const = default;
};

/// Wire shape of a PolicyRule. Mirrors firmius::core::PolicyRule.
struct PolicyRuleWire {
  std::string id;
  std::string category;
  std::string decision;        ///< "allow" | "deny" | "ask"
  std::string scope;           ///< "global" | "project" | "session"
  std::string comment;
  std::map<std::string, std::string> match;
  std::uint64_t createdAt = 0;
  std::uint64_t expiresAt = 0;

  bool operator==(const PolicyRuleWire &) const = default;
};

struct PermissionListRulesRequest {
  bool operator==(const PermissionListRulesRequest &) const = default;
};

struct PermissionListRulesResponse {
  std::vector<PolicyRuleWire> rules;
  /// Per-category default decisions ("allow" | "deny" | "ask").
  std::map<std::string, std::string> categoryDefaults;
  std::string defaultDecision = "ask";

  bool operator==(const PermissionListRulesResponse &) const = default;
};

struct PermissionUpsertRuleRequest {
  PolicyRuleWire rule;

  bool operator==(const PermissionUpsertRuleRequest &) const = default;
};

struct PermissionUpsertRuleResponse {
  std::string ruleId;          ///< Persisted id (may differ from input).
  std::string errorMessage;    ///< Empty on success.

  bool operator==(const PermissionUpsertRuleResponse &) const = default;
};

struct PermissionDeleteRuleRequest {
  std::string ruleId;

  bool operator==(const PermissionDeleteRuleRequest &) const = default;
};

struct PermissionDeleteRuleResponse {
  bool removed = false;
  std::string errorMessage;

  bool operator==(const PermissionDeleteRuleResponse &) const = default;
};

struct PermissionReloadPolicyRequest {
  bool operator==(const PermissionReloadPolicyRequest &) const = default;
};

struct PermissionReloadPolicyResponse {
  bool ok = true;
  std::string errorMessage;

  bool operator==(const PermissionReloadPolicyResponse &) const = default;
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

struct ThreadOverviewRequest {
  std::string cwd;

  bool operator==(const ThreadOverviewRequest &) const = default;
};

struct ThreadOverview {
  firmius::shared::ThreadMetadata thread;
  std::size_t agentCount = 0;
  std::size_t artifactCount = 0;
  int lockOwnerPid = 0;
  std::string lockOwnerClientId;
  std::string lockOwnerUiKind;
  bool lockedByOtherClient = false;

  bool operator==(const ThreadOverview &) const = default;
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

using AgentTodoSnapshot = firmius::shared::AgentTodoList;

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
  /// Currently-active mode id (e.g. "ask", "yolo", or user-defined).
  std::string activeModeId;
  /// Snapshot of every available mode (for the picker). Includes
  /// built-ins and user-created ones.
  std::vector<PermissionModeWire> modes;
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

// ── /connect wizard protocol ───────────────────────────────────────────────
//
// /connect runs an interactive provider authentication flow. The wizard lives
// in the daemon (since the daemon owns ProviderRegistry); the TUI is a thin
// client that renders prompts and forwards answers.
//
// Lifecycle:
//   1. client → connect.begin {provider_id}
//        - if accounts already exist and add_additional=false, the daemon
//          replies with existing_accounts=true and NO session_id; the TUI
//          shows a confirm prompt then re-calls with add_additional=true
//        - otherwise the daemon spins up a wizard and returns the first prompt
//   2. client → connect.submit {session_id, answer}  (repeat per prompt)
//   3. when next_prompt is empty AND ready_to_finalize=true:
//        client → connect.finalize {session_id}
//        - returns immediately; daemon spawns a worker that polls
//          isComplete() then runs finalizeExchange()
//        - daemon emits DaemonEventKind::ConnectProgress events as the worker
//          progresses (Polling → Finalizing → Succeeded/Failed)
//   4. on cancel (ESC) or client disconnect: connect.cancel {session_id}
//
// Constraints:
//   - One active wizard per client. Calling connect.begin a second time
//     auto-cancels the prior session.
//   - ConnectProgress events are delivered ONLY to the owning client.

struct WizardChoiceSnapshot {
  std::string label;
  std::string value;

  bool operator==(const WizardChoiceSnapshot &) const = default;
};

struct WizardPromptSnapshot {
  std::string message;
  bool isSecret = false;
  std::vector<WizardChoiceSnapshot> choices;
  bool allowFreeformInput = true;
  bool allowEmptyInput = false;
  std::string placeholder;
  std::string submitLabel;
  /// First https?:// URL detected in `message`. Server-side parity with v1's
  /// modal so the TUI can offer a one-shot "open in browser" affordance.
  std::string detectedUrl;
  /// Synthetic "waiting" prompt shown while a long-running OAuth flow is
  /// pending in the wizard. The TUI renders it like any other prompt; no
  /// special branch needed.
  bool isWaiting = false;

  bool operator==(const WizardPromptSnapshot &) const = default;
};

struct ConnectBeginRequest {
  std::string providerId;
  /// Set true after the user confirms "an account already exists, add another?"
  bool addAdditional = false;

  bool operator==(const ConnectBeginRequest &) const = default;
};

struct ConnectBeginResponse {
  std::string sessionId;
  std::string providerId;
  std::string providerKind;     // "oauth" | "apikey"
  /// True when the provider already has accounts AND addAdditional was false.
  /// In this case sessionId is empty and the TUI must confirm + retry.
  bool existingAccounts = false;
  std::optional<WizardPromptSnapshot> prompt;
  /// True when the wizard finished synchronously (no prompts at all).
  bool readyToFinalize = false;
  /// Populated on error (e.g. unknown provider id).
  std::string errorMessage;

  bool operator==(const ConnectBeginResponse &) const = default;
};

struct ConnectSubmitRequest {
  std::string sessionId;
  std::string answer;

  bool operator==(const ConnectSubmitRequest &) const = default;
};

struct ConnectSubmitResponse {
  bool ok = false;
  std::optional<WizardPromptSnapshot> prompt;
  bool readyToFinalize = false;
  std::string errorMessage;

  bool operator==(const ConnectSubmitResponse &) const = default;
};

struct ConnectFinalizeRequest {
  std::string sessionId;

  bool operator==(const ConnectFinalizeRequest &) const = default;
};

struct ConnectFinalizeResponse {
  /// `accepted=true` means the daemon kicked off the finalize worker.
  /// The actual success/failure arrives via a ConnectProgress event with
  /// phase=Succeeded or phase=Failed.
  bool accepted = false;
  std::string errorMessage;

  bool operator==(const ConnectFinalizeResponse &) const = default;
};

struct ConnectCancelRequest {
  std::string sessionId;

  bool operator==(const ConnectCancelRequest &) const = default;
};

struct ConnectCancelResponse {
  bool cancelled = false;

  bool operator==(const ConnectCancelResponse &) const = default;
};

enum class ConnectProgressPhase {
  Polling,     ///< wizard not yet complete (waiting on browser flow, etc.)
  Finalizing,  ///< isComplete() true, running finalizeExchange()
  Succeeded,
  Failed,
  Cancelled,
};

struct ConnectProgressSnapshot {
  std::string sessionId;
  std::string providerId;
  ConnectProgressPhase phase = ConnectProgressPhase::Polling;
  /// On Succeeded: wizard->getFinalMessage().
  /// On Failed:    error from finalizeExchange().
  /// On Polling:   optional human-readable hint.
  std::string message;

  bool operator==(const ConnectProgressSnapshot &) const = default;
};

// ── /undo Rewind protocol ────────────────────────────────────────────────
//
// Claude-Code-style rewind:
//   1. /undo opens an overlay listing all user-message turns of the focused
//      agent's transcript.
//   2. As the user highlights a row, the TUI calls rewind.preview(turnId)
//      to learn (a) how many turns would be discarded, (b) which edit
//      batches would be rolled back, (c) per-batch undo eligibility.
//   3. User picks a mode: restore code+conversation, conversation only,
//      code only, or never-mind. TUI calls rewind.execute(turnId, mode).
//   4. Daemon performs a compound undo (edit batches in reverse order,
//      then transcript turns), persists a single TranscriptUndoAction
//      whose editUndoActionIds list is populated, then emits
//      DaemonEventKind::RewindApplied so all subscribed clients can
//      refresh their transcript.

enum class RewindMode {
  /// Undo edits authored after target turn AND undo all turns after it.
  /// This is the default; matches "rewind everything".
  RestoreCodeAndConversation,
  /// Undo only the transcript turns. Files left as-is.
  RestoreConversation,
  /// Undo only the edit batches. Transcript stays.
  RestoreCode,
};

struct RewindPreviewRequest {
  std::string threadId;
  std::string agentId;
  /// User-message turn ID the user wants to roll back to. The chosen turn
  /// itself is preserved; everything strictly after it gets discarded.
  std::string targetTurnId;

  bool operator==(const RewindPreviewRequest &) const = default;
};

struct RewindPreviewResponse {
  std::string threadId;
  std::string agentId;
  std::string targetTurnId;
  /// Number of turns that would be discarded. ≥ 1 when valid.
  int turnsToUndo = 0;
  /// 1-line preview of the user message at targetTurnId.
  std::string targetMessagePreview;
  std::uint64_t targetMessageCreatedAt = 0;

  /// Edit batches authored AFTER the target turn (those that would be
  /// rolled back if the user picks RestoreCode/RestoreCodeAndConversation).
  /// Aligned 1:1 with editEligibilities.
  std::vector<firmius::shared::EditBatchSummary> affectedEditBatches;
  std::vector<firmius::shared::EditUndoEligibility> editEligibilities;

  /// Convenience aggregates for the overlay header line ("+X/-Y across N").
  int totalAddedLines = 0;
  int totalRemovedLines = 0;
  std::vector<std::string> filesAffected;

  /// True iff every batch in affectedEditBatches is undoable right now.
  /// When false, the overlay should grey out the code-restore modes and
  /// surface codeRestoreBlockReason.
  bool codeRestoreSafe = true;
  std::string codeRestoreBlockReason;

  /// Populated on validation failure (unknown turn id, etc.). When set,
  /// turnsToUndo is 0 and the overlay should show this instead of the
  /// preview pane.
  std::string errorMessage;

  bool operator==(const RewindPreviewResponse &) const = default;
};

struct RewindExecuteRequest {
  std::string threadId;
  std::string agentId;
  std::string targetTurnId;
  RewindMode mode = RewindMode::RestoreCodeAndConversation;

  bool operator==(const RewindExecuteRequest &) const = default;
};

struct RewindExecuteResponse {
  bool applied = false;
  std::string undoActionId;
  std::vector<std::string> editUndoActionIds;
  std::string errorMessage;

  bool operator==(const RewindExecuteResponse &) const = default;
};

/// Broadcast event when a rewind completes so all clients viewing the
/// thread can refresh. Carries enough detail for a future "show what was
/// rolled back" toast.
struct RewindAppliedSnapshot {
  std::string threadId;
  std::string agentId;
  std::string targetTurnId;
  RewindMode mode = RewindMode::RestoreCodeAndConversation;
  int turnsUndone = 0;
  int editBatchesUndone = 0;
  std::string undoActionId;

  bool operator==(const RewindAppliedSnapshot &) const = default;
};

// ── /redo flow ───────────────────────────────────────────────────────
//
// Forward of the rewind flow. After /undo, the daemon persists a
// TranscriptUndoAction with a captured payload of discarded turns +
// linked editUndoActionIds. /redo replays them.
//
// The same three modes apply: code-only, conversation-only, both. The
// availability of each is gated by the undo action's redoAvailable
// flag and whether editUndoActionIds is non-empty.

enum class RedoMode {
  RestoreCodeAndConversation,
  RestoreConversation,
  RestoreCode,
};

struct RedoUndoActionSummary {
  std::string undoActionId;
  std::uint64_t createdAt = 0;
  /// Number of turns that would be re-appended.
  int turnsToRedo = 0;
  /// One-line preview of the first restored turn (user-message text or
  /// a short tool-call summary).
  std::string firstTurnPreview;
  /// Number of edit batches that would be re-applied.
  int editBatchesToRedo = 0;
  /// Whether redo is still available (false if the user already
  /// triggered redo previously, or if the daemon decided to expire it).
  bool redoAvailable = false;

  bool operator==(const RedoUndoActionSummary &) const = default;
};

struct RedoPreviewRequest {
  std::string threadId;
  std::string agentId;
  /// Maximum number of recent undo actions to return. Defaults to 10
  /// because the picker shows newest-first and most users only need
  /// the last few.
  int limit = 10;

  bool operator==(const RedoPreviewRequest &) const = default;
};

struct RedoPreviewResponse {
  std::string threadId;
  std::string agentId;
  /// Newest-first list of recent undo actions the user can pick from.
  std::vector<RedoUndoActionSummary> actions;
  std::string errorMessage;

  bool operator==(const RedoPreviewResponse &) const = default;
};

struct RedoExecuteRequest {
  std::string threadId;
  std::string agentId;
  std::string undoActionId;
  RedoMode mode = RedoMode::RestoreCodeAndConversation;

  bool operator==(const RedoExecuteRequest &) const = default;
};

struct RedoExecuteResponse {
  bool applied = false;
  int turnsRedone = 0;
  std::vector<std::string> editRedoActionIds;
  std::string errorMessage;

  bool operator==(const RedoExecuteResponse &) const = default;
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
  std::optional<ConnectProgressSnapshot> connectProgress;
  std::optional<RewindAppliedSnapshot> rewindApplied;

  bool operator==(const DaemonEventEnvelope &) const = default;
};

} // namespace firmius::daemon

#endif // FIRMIUS_SERVER_PROTOCOL_HPP

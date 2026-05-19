#ifndef FIRMIUS_SERVER_PROTOCOLSERIALIZATION_HPP
#define FIRMIUS_SERVER_PROTOCOLSERIALIZATION_HPP

#include "daemon/Protocol.hpp"
#include "Serialization.hpp"

#include <optional>
#include <rapidjson/document.h>
#include <string>

namespace firmius::daemon {

rapidjson::Value toJsonValue(const WorkspacePresence &presence,
                             rapidjson::Document::AllocatorType &allocator);
WorkspacePresence workspacePresenceFromJson(const rapidjson::Value &value);

rapidjson::Value toJsonValue(const ClientIdentity &identity,
                             rapidjson::Document::AllocatorType &allocator);
ClientIdentity clientIdentityFromJson(const rapidjson::Value &value);

rapidjson::Value toJsonValue(const ClientSessionSnapshot &session,
                             rapidjson::Document::AllocatorType &allocator);
ClientSessionSnapshot clientSessionSnapshotFromJson(const rapidjson::Value &value);

rapidjson::Value toJsonValue(const DaemonPingResponse &response,
                             rapidjson::Document::AllocatorType &allocator);
DaemonPingResponse daemonPingResponseFromJson(const rapidjson::Value &value);
rapidjson::Value toJsonValue(const DaemonAuditEmitRuntimeEventRequest &request,
                             rapidjson::Document::AllocatorType &allocator);
DaemonAuditEmitRuntimeEventRequest
daemonAuditEmitRuntimeEventRequestFromJson(const rapidjson::Value &value);
rapidjson::Value toJsonValue(const DaemonAuditEmitRuntimeEventResponse &response,
                             rapidjson::Document::AllocatorType &allocator);
DaemonAuditEmitRuntimeEventResponse
daemonAuditEmitRuntimeEventResponseFromJson(const rapidjson::Value &value);

rapidjson::Value toJsonValue(const ClientHelloRequest &request,
                             rapidjson::Document::AllocatorType &allocator);
ClientHelloRequest clientHelloRequestFromJson(const rapidjson::Value &value);

rapidjson::Value toJsonValue(const ClientHelloResponse &response,
                             rapidjson::Document::AllocatorType &allocator);
ClientHelloResponse clientHelloResponseFromJson(const rapidjson::Value &value);

rapidjson::Value toJsonValue(const ThreadsOpenResponse &response,
                             rapidjson::Document::AllocatorType &allocator);
ThreadsOpenResponse threadsOpenResponseFromJson(const rapidjson::Value &value);
rapidjson::Value toJsonValue(const ThreadSnapshot &snapshot,
                             rapidjson::Document::AllocatorType &allocator);
ThreadSnapshot threadSnapshotFromJson(const rapidjson::Value &value);

rapidjson::Value toJsonValue(const ThreadsCreateRequest &request,
                             rapidjson::Document::AllocatorType &allocator);
ThreadsCreateRequest threadsCreateRequestFromJson(const rapidjson::Value &value);

rapidjson::Value toJsonValue(const ThreadsCreateResponse &response,
                             rapidjson::Document::AllocatorType &allocator);
ThreadsCreateResponse threadsCreateResponseFromJson(const rapidjson::Value &value);

rapidjson::Value toJsonValue(const ThreadsSendRequest &request,
                             rapidjson::Document::AllocatorType &allocator);
ThreadsSendRequest threadsSendRequestFromJson(const rapidjson::Value &value);

rapidjson::Value toJsonValue(const ThreadsSendResponse &response,
                             rapidjson::Document::AllocatorType &allocator);
ThreadsSendResponse threadsSendResponseFromJson(const rapidjson::Value &value);
rapidjson::Value toJsonValue(const ThreadOverviewRequest &request,
                             rapidjson::Document::AllocatorType &allocator);
ThreadOverviewRequest threadOverviewRequestFromJson(const rapidjson::Value &value);
rapidjson::Value toJsonValue(const ThreadOverview &snapshot,
                             rapidjson::Document::AllocatorType &allocator);
ThreadOverview threadOverviewFromJson(const rapidjson::Value &value);

rapidjson::Value toJsonValue(const AgentTargetRequest &request,
                             rapidjson::Document::AllocatorType &allocator);
AgentTargetRequest agentTargetRequestFromJson(const rapidjson::Value &value);
rapidjson::Value toJsonValue(const AgentRuntimeSnapshot &snapshot,
                             rapidjson::Document::AllocatorType &allocator);
AgentRuntimeSnapshot agentRuntimeSnapshotFromJson(const rapidjson::Value &value);
rapidjson::Value toJsonValue(const AgentTodoSnapshot &snapshot,
                             rapidjson::Document::AllocatorType &allocator);
AgentTodoSnapshot agentTodoSnapshotFromJson(const rapidjson::Value &value);
rapidjson::Value toJsonValue(const AgentTreeSnapshot &snapshot,
                             rapidjson::Document::AllocatorType &allocator);
AgentTreeSnapshot agentTreeSnapshotFromJson(const rapidjson::Value &value);

rapidjson::Value toJsonValue(const ProcessesListRequest &request,
                             rapidjson::Document::AllocatorType &allocator);
ProcessesListRequest processesListRequestFromJson(const rapidjson::Value &value);
rapidjson::Value toJsonValue(const ProcessesGetRequest &request,
                             rapidjson::Document::AllocatorType &allocator);
ProcessesGetRequest processesGetRequestFromJson(const rapidjson::Value &value);
rapidjson::Value toJsonValue(const ProcessSnapshot &snapshot,
                             rapidjson::Document::AllocatorType &allocator);
ProcessSnapshot processSnapshotFromJson(const rapidjson::Value &value);
rapidjson::Value toJsonValue(const ProcessRuntimeSummary &summary,
                             rapidjson::Document::AllocatorType &allocator);
ProcessRuntimeSummary processRuntimeSummaryFromJson(const rapidjson::Value &value);

rapidjson::Value toJsonValue(const PermissionModeWire &mode,
                             rapidjson::Document::AllocatorType &allocator);
PermissionModeWire permissionModeWireFromJson(const rapidjson::Value &value);
rapidjson::Value toJsonValue(const PermissionModeRequest &request,
                             rapidjson::Document::AllocatorType &allocator);
PermissionModeRequest permissionModeRequestFromJson(const rapidjson::Value &value);
rapidjson::Value toJsonValue(const PermissionModeUpdateRequest &request,
                             rapidjson::Document::AllocatorType &allocator);
PermissionModeUpdateRequest permissionModeUpdateRequestFromJson(const rapidjson::Value &value);
rapidjson::Value toJsonValue(const PermissionCreateModeRequest &request,
                             rapidjson::Document::AllocatorType &allocator);
PermissionCreateModeRequest permissionCreateModeRequestFromJson(const rapidjson::Value &value);
rapidjson::Value toJsonValue(const PermissionCreateModeResponse &response,
                             rapidjson::Document::AllocatorType &allocator);
PermissionCreateModeResponse permissionCreateModeResponseFromJson(const rapidjson::Value &value);
rapidjson::Value toJsonValue(const PermissionRenameModeRequest &request,
                             rapidjson::Document::AllocatorType &allocator);
PermissionRenameModeRequest permissionRenameModeRequestFromJson(const rapidjson::Value &value);
rapidjson::Value toJsonValue(const PermissionRenameModeResponse &response,
                             rapidjson::Document::AllocatorType &allocator);
PermissionRenameModeResponse permissionRenameModeResponseFromJson(const rapidjson::Value &value);
rapidjson::Value toJsonValue(const PermissionDeleteModeRequest &request,
                             rapidjson::Document::AllocatorType &allocator);
PermissionDeleteModeRequest permissionDeleteModeRequestFromJson(const rapidjson::Value &value);
rapidjson::Value toJsonValue(const PermissionDeleteModeResponse &response,
                             rapidjson::Document::AllocatorType &allocator);
PermissionDeleteModeResponse permissionDeleteModeResponseFromJson(const rapidjson::Value &value);
rapidjson::Value toJsonValue(const PermissionResolveRequest &request,
                             rapidjson::Document::AllocatorType &allocator);
PermissionResolveRequest permissionResolveRequestFromJson(const rapidjson::Value &value);
rapidjson::Value toJsonValue(const PermissionResolveWithRulesRequest &request,
                             rapidjson::Document::AllocatorType &allocator);
PermissionResolveWithRulesRequest
permissionResolveWithRulesRequestFromJson(const rapidjson::Value &value);
rapidjson::Value toJsonValue(const PolicyRuleWire &rule,
                             rapidjson::Document::AllocatorType &allocator);
PolicyRuleWire policyRuleWireFromJson(const rapidjson::Value &value);
rapidjson::Value toJsonValue(const PermissionListRulesRequest &request,
                             rapidjson::Document::AllocatorType &allocator);
PermissionListRulesRequest permissionListRulesRequestFromJson(const rapidjson::Value &value);
rapidjson::Value toJsonValue(const PermissionListRulesResponse &response,
                             rapidjson::Document::AllocatorType &allocator);
PermissionListRulesResponse permissionListRulesResponseFromJson(const rapidjson::Value &value);
rapidjson::Value toJsonValue(const PermissionUpsertRuleRequest &request,
                             rapidjson::Document::AllocatorType &allocator);
PermissionUpsertRuleRequest permissionUpsertRuleRequestFromJson(const rapidjson::Value &value);
rapidjson::Value toJsonValue(const PermissionUpsertRuleResponse &response,
                             rapidjson::Document::AllocatorType &allocator);
PermissionUpsertRuleResponse permissionUpsertRuleResponseFromJson(const rapidjson::Value &value);
rapidjson::Value toJsonValue(const PermissionDeleteRuleRequest &request,
                             rapidjson::Document::AllocatorType &allocator);
PermissionDeleteRuleRequest permissionDeleteRuleRequestFromJson(const rapidjson::Value &value);
rapidjson::Value toJsonValue(const PermissionDeleteRuleResponse &response,
                             rapidjson::Document::AllocatorType &allocator);
PermissionDeleteRuleResponse permissionDeleteRuleResponseFromJson(const rapidjson::Value &value);
rapidjson::Value toJsonValue(const PermissionReloadPolicyRequest &request,
                             rapidjson::Document::AllocatorType &allocator);
PermissionReloadPolicyRequest permissionReloadPolicyRequestFromJson(const rapidjson::Value &value);
rapidjson::Value toJsonValue(const PermissionReloadPolicyResponse &response,
                             rapidjson::Document::AllocatorType &allocator);
PermissionReloadPolicyResponse permissionReloadPolicyResponseFromJson(const rapidjson::Value &value);
rapidjson::Value toJsonValue(const PermissionQueueSnapshot &snapshot,
                             rapidjson::Document::AllocatorType &allocator);
PermissionQueueSnapshot permissionQueueSnapshotFromJson(const rapidjson::Value &value);

rapidjson::Value toJsonValue(const ModelSwitchRequest &request,
                             rapidjson::Document::AllocatorType &allocator);
ModelSwitchRequest modelSwitchRequestFromJson(const rapidjson::Value &value);
rapidjson::Value toJsonValue(const ModelCatalogSnapshot &snapshot,
                             rapidjson::Document::AllocatorType &allocator);
ModelCatalogSnapshot modelCatalogSnapshotFromJson(const rapidjson::Value &value);

rapidjson::Value toJsonValue(const ProviderCatalogSnapshot &snapshot,
                             rapidjson::Document::AllocatorType &allocator);
ProviderCatalogSnapshot providerCatalogSnapshotFromJson(const rapidjson::Value &value);
rapidjson::Value toJsonValue(const ProviderProfilesUpdateRequest &request,
                             rapidjson::Document::AllocatorType &allocator);
ProviderProfilesUpdateRequest providerProfilesUpdateRequestFromJson(const rapidjson::Value &value);
rapidjson::Value toJsonValue(const UserConfigSnapshot &snapshot,
                             rapidjson::Document::AllocatorType &allocator);
UserConfigSnapshot userConfigSnapshotFromJson(const rapidjson::Value &value);
rapidjson::Value toJsonValue(const ConfigUpdateRequest &request,
                             rapidjson::Document::AllocatorType &allocator);
ConfigUpdateRequest configUpdateRequestFromJson(const rapidjson::Value &value);
rapidjson::Value toJsonValue(const HistoryGetRequest &request,
                             rapidjson::Document::AllocatorType &allocator);
HistoryGetRequest historyGetRequestFromJson(const rapidjson::Value &value);
rapidjson::Value toJsonValue(const HistoryUndoRequest &request,
                             rapidjson::Document::AllocatorType &allocator);
HistoryUndoRequest historyUndoRequestFromJson(const rapidjson::Value &value);
rapidjson::Value toJsonValue(const HistoryRedoRequest &request,
                             rapidjson::Document::AllocatorType &allocator);
HistoryRedoRequest historyRedoRequestFromJson(const rapidjson::Value &value);
rapidjson::Value toJsonValue(const HistorySnapshot &snapshot,
                             rapidjson::Document::AllocatorType &allocator);
HistorySnapshot historySnapshotFromJson(const rapidjson::Value &value);
rapidjson::Value toJsonValue(const HistoryMutationResult &result,
                             rapidjson::Document::AllocatorType &allocator);
HistoryMutationResult historyMutationResultFromJson(const rapidjson::Value &value);
rapidjson::Value toJsonValue(const RouterConfigSnapshot &snapshot,
                             rapidjson::Document::AllocatorType &allocator);
RouterConfigSnapshot routerConfigSnapshotFromJson(const rapidjson::Value &value);
rapidjson::Value toJsonValue(const RouterConfigUpdateRequest &request,
                             rapidjson::Document::AllocatorType &allocator);
RouterConfigUpdateRequest routerConfigUpdateRequestFromJson(const rapidjson::Value &value);
rapidjson::Value toJsonValue(const PurposesConfigSnapshot &snapshot,
                             rapidjson::Document::AllocatorType &allocator);
PurposesConfigSnapshot purposesConfigSnapshotFromJson(const rapidjson::Value &value);
rapidjson::Value toJsonValue(const PurposesConfigUpdateRequest &request,
                             rapidjson::Document::AllocatorType &allocator);
PurposesConfigUpdateRequest purposesConfigUpdateRequestFromJson(const rapidjson::Value &value);
rapidjson::Value toJsonValue(const McpConfigSnapshot &snapshot,
                             rapidjson::Document::AllocatorType &allocator);
McpConfigSnapshot mcpConfigSnapshotFromJson(const rapidjson::Value &value);
rapidjson::Value toJsonValue(const McpConfigUpdateRequest &request,
                             rapidjson::Document::AllocatorType &allocator);
McpConfigUpdateRequest mcpConfigUpdateRequestFromJson(const rapidjson::Value &value);

rapidjson::Value toJsonValue(const AccountsRequest &request,
                             rapidjson::Document::AllocatorType &allocator);
AccountsRequest accountsRequestFromJson(const rapidjson::Value &value);
rapidjson::Value toJsonValue(const AccountDeleteRequest &request,
                             rapidjson::Document::AllocatorType &allocator);
AccountDeleteRequest accountDeleteRequestFromJson(const rapidjson::Value &value);
rapidjson::Value toJsonValue(const AccountSnapshot &snapshot,
                             rapidjson::Document::AllocatorType &allocator);
AccountSnapshot accountSnapshotFromJson(const rapidjson::Value &value);

rapidjson::Value toJsonValue(const QuotasRequest &request,
                             rapidjson::Document::AllocatorType &allocator);
QuotasRequest quotasRequestFromJson(const rapidjson::Value &value);
rapidjson::Value toJsonValue(const QuotaSnapshot &snapshot,
                             rapidjson::Document::AllocatorType &allocator);
QuotaSnapshot quotaSnapshotFromJson(const rapidjson::Value &value);

rapidjson::Value toJsonValue(const HooksStateRequest &request,
                             rapidjson::Document::AllocatorType &allocator);
HooksStateRequest hooksStateRequestFromJson(const rapidjson::Value &value);
rapidjson::Value toJsonValue(const HookStatusSnapshot &snapshot,
                             rapidjson::Document::AllocatorType &allocator);
HookStatusSnapshot hookStatusSnapshotFromJson(const rapidjson::Value &value);
rapidjson::Value toJsonValue(const HookStateSnapshot &snapshot,
                             rapidjson::Document::AllocatorType &allocator);
HookStateSnapshot hookStateSnapshotFromJson(const rapidjson::Value &value);
rapidjson::Value toJsonValue(const HooksRecentActivitySnapshot &snapshot,
                             rapidjson::Document::AllocatorType &allocator);
HooksRecentActivitySnapshot
hooksRecentActivitySnapshotFromJson(const rapidjson::Value &value);
rapidjson::Value toJsonValue(const PactsListRequest &request,
                             rapidjson::Document::AllocatorType &allocator);
PactsListRequest pactsListRequestFromJson(const rapidjson::Value &value);
rapidjson::Value toJsonValue(const PactsGetRequest &request,
                             rapidjson::Document::AllocatorType &allocator);
PactsGetRequest pactsGetRequestFromJson(const rapidjson::Value &value);
rapidjson::Value toJsonValue(const PactHistoryEntrySnapshot &snapshot,
                             rapidjson::Document::AllocatorType &allocator);
PactHistoryEntrySnapshot pactHistoryEntrySnapshotFromJson(const rapidjson::Value &value);
rapidjson::Value toJsonValue(const PactSnapshot &snapshot,
                             rapidjson::Document::AllocatorType &allocator);
PactSnapshot pactSnapshotFromJson(const rapidjson::Value &value);

rapidjson::Value toJsonValue(const WorkflowExecuteRequest &request,
                             rapidjson::Document::AllocatorType &allocator);
WorkflowExecuteRequest workflowExecuteRequestFromJson(const rapidjson::Value &value);
rapidjson::Value toJsonValue(const WorkflowExecutionSnapshot &snapshot,
                             rapidjson::Document::AllocatorType &allocator);
WorkflowExecutionSnapshot workflowExecutionSnapshotFromJson(const rapidjson::Value &value);

rapidjson::Value toJsonValue(const ArtifactCatalogSnapshot &snapshot,
                             rapidjson::Document::AllocatorType &allocator);
ArtifactCatalogSnapshot artifactCatalogSnapshotFromJson(const rapidjson::Value &value);
rapidjson::Value toJsonValue(const EditsListRequest &request,
                             rapidjson::Document::AllocatorType &allocator);
EditsListRequest editsListRequestFromJson(const rapidjson::Value &value);
rapidjson::Value toJsonValue(const EditsUndoRequest &request,
                             rapidjson::Document::AllocatorType &allocator);
EditsUndoRequest editsUndoRequestFromJson(const rapidjson::Value &value);
rapidjson::Value toJsonValue(const EditsRedoRequest &request,
                             rapidjson::Document::AllocatorType &allocator);
EditsRedoRequest editsRedoRequestFromJson(const rapidjson::Value &value);
rapidjson::Value toJsonValue(const EditHistorySnapshot &snapshot,
                             rapidjson::Document::AllocatorType &allocator);
EditHistorySnapshot editHistorySnapshotFromJson(const rapidjson::Value &value);
rapidjson::Value toJsonValue(const EditMutationResult &result,
                             rapidjson::Document::AllocatorType &allocator);
EditMutationResult editMutationResultFromJson(const rapidjson::Value &value);
rapidjson::Value toJsonValue(const TranscriptGetRequest &request,
                             rapidjson::Document::AllocatorType &allocator);
TranscriptGetRequest transcriptGetRequestFromJson(const rapidjson::Value &value);
rapidjson::Value toJsonValue(const TranscriptSnapshot &snapshot,
                             rapidjson::Document::AllocatorType &allocator);
TranscriptSnapshot transcriptSnapshotFromJson(const rapidjson::Value &value);
rapidjson::Value toJsonValue(const ToolCallsListRequest &request,
                             rapidjson::Document::AllocatorType &allocator);
ToolCallsListRequest toolCallsListRequestFromJson(const rapidjson::Value &value);
rapidjson::Value toJsonValue(const ToolCallSnapshot &snapshot,
                             rapidjson::Document::AllocatorType &allocator);
ToolCallSnapshot toolCallSnapshotFromJson(const rapidjson::Value &value);
rapidjson::Value toJsonValue(const SubagentsActivityRequest &request,
                             rapidjson::Document::AllocatorType &allocator);
SubagentsActivityRequest subagentsActivityRequestFromJson(const rapidjson::Value &value);
rapidjson::Value toJsonValue(const SubagentActivityLogEntrySnapshot &snapshot,
                             rapidjson::Document::AllocatorType &allocator);
SubagentActivityLogEntrySnapshot subagentActivityLogEntrySnapshotFromJson(const rapidjson::Value &value);
rapidjson::Value toJsonValue(const SubagentActivityEntrySnapshot &snapshot,
                             rapidjson::Document::AllocatorType &allocator);
SubagentActivityEntrySnapshot subagentActivityEntrySnapshotFromJson(const rapidjson::Value &value);
rapidjson::Value toJsonValue(const SubagentActivitySnapshot &snapshot,
                             rapidjson::Document::AllocatorType &allocator);
SubagentActivitySnapshot subagentActivitySnapshotFromJson(const rapidjson::Value &value);

rapidjson::Value toJsonValue(const BenchmarksStartRequest &request,
                             rapidjson::Document::AllocatorType &allocator);
BenchmarksStartRequest benchmarksStartRequestFromJson(const rapidjson::Value &value);
rapidjson::Value toJsonValue(const BenchmarksStartResponse &response,
                             rapidjson::Document::AllocatorType &allocator);
BenchmarksStartResponse benchmarksStartResponseFromJson(const rapidjson::Value &value);

rapidjson::Value toJsonValue(const BenchmarksStatusRequest &request,
                             rapidjson::Document::AllocatorType &allocator);
BenchmarksStatusRequest benchmarksStatusRequestFromJson(const rapidjson::Value &value);
rapidjson::Value toJsonValue(const BenchmarkStatusSnapshot &snapshot,
                             rapidjson::Document::AllocatorType &allocator);
BenchmarkStatusSnapshot benchmarkStatusSnapshotFromJson(const rapidjson::Value &value);

rapidjson::Value toJsonValue(const BenchmarksLogsRequest &request,
                             rapidjson::Document::AllocatorType &allocator);
BenchmarksLogsRequest benchmarksLogsRequestFromJson(const rapidjson::Value &value);
rapidjson::Value toJsonValue(const BenchmarkLogsSnapshot &snapshot,
                             rapidjson::Document::AllocatorType &allocator);
BenchmarkLogsSnapshot benchmarkLogsSnapshotFromJson(const rapidjson::Value &value);

rapidjson::Value toJsonValue(const UiSnapshotRequest &request,
                             rapidjson::Document::AllocatorType &allocator);
UiSnapshotRequest uiSnapshotRequestFromJson(const rapidjson::Value &value);
rapidjson::Value toJsonValue(const UiSnapshot &snapshot,
                             rapidjson::Document::AllocatorType &allocator);
UiSnapshot uiSnapshotFromJson(const rapidjson::Value &value);
rapidjson::Value toJsonValue(const EventSubscriptionRequest &request,
                             rapidjson::Document::AllocatorType &allocator);
EventSubscriptionRequest eventSubscriptionRequestFromJson(const rapidjson::Value &value);

rapidjson::Value toJsonValue(const EventSubscriptionResponse &response,
                             rapidjson::Document::AllocatorType &allocator);
EventSubscriptionResponse eventSubscriptionResponseFromJson(const rapidjson::Value &value);

rapidjson::Value toJsonValue(const DaemonEventEnvelope &event,
                             rapidjson::Document::AllocatorType &allocator);
DaemonEventEnvelope daemonEventEnvelopeFromJson(const rapidjson::Value &value);

rapidjson::Value toJsonValue(const std::vector<ClientSessionSnapshot> &sessions,
                             rapidjson::Document::AllocatorType &allocator);
std::vector<ClientSessionSnapshot> clientSessionSnapshotListFromJson(const rapidjson::Value &value);

rapidjson::Value toJsonValue(const std::vector<firmius::shared::ThreadMetadata> &threads,
                             rapidjson::Document::AllocatorType &allocator);
std::vector<firmius::shared::ThreadMetadata> threadMetadataListFromJson(const rapidjson::Value &value);
rapidjson::Value toJsonValue(const std::vector<ThreadOverview> &threads,
                             rapidjson::Document::AllocatorType &allocator);
std::vector<ThreadOverview> threadOverviewListFromJson(const rapidjson::Value &value);
rapidjson::Value toJsonValue(const std::vector<AgentRuntimeSnapshot> &snapshots,
                             rapidjson::Document::AllocatorType &allocator);
std::vector<AgentRuntimeSnapshot> agentRuntimeSnapshotListFromJson(const rapidjson::Value &value);
rapidjson::Value toJsonValue(const std::vector<ProcessSnapshot> &snapshots,
                             rapidjson::Document::AllocatorType &allocator);
std::vector<ProcessSnapshot> processSnapshotListFromJson(const rapidjson::Value &value);
rapidjson::Value toJsonValue(const std::vector<AccountSnapshot> &snapshots,
                             rapidjson::Document::AllocatorType &allocator);
std::vector<AccountSnapshot> accountSnapshotListFromJson(const rapidjson::Value &value);
rapidjson::Value toJsonValue(const std::vector<WorkflowExecutionSnapshot> &snapshots,
                             rapidjson::Document::AllocatorType &allocator);
std::vector<WorkflowExecutionSnapshot> workflowExecutionSnapshotListFromJson(const rapidjson::Value &value);
rapidjson::Value toJsonValue(const std::vector<PactSnapshot> &snapshots,
                             rapidjson::Document::AllocatorType &allocator);
std::vector<PactSnapshot> pactSnapshotListFromJson(const rapidjson::Value &value);
rapidjson::Value toJsonValue(const std::vector<ToolCallSnapshot> &snapshots,
                             rapidjson::Document::AllocatorType &allocator);
std::vector<ToolCallSnapshot> toolCallSnapshotListFromJson(const rapidjson::Value &value);
rapidjson::Value toJsonValue(
    const std::vector<SubagentActivityEntrySnapshot> &snapshots,
    rapidjson::Document::AllocatorType &allocator);
std::vector<SubagentActivityEntrySnapshot>
subagentActivityEntrySnapshotListFromJson(const rapidjson::Value &value);

rapidjson::Value toJsonValue(const ModesListRequest &r, rapidjson::Document::AllocatorType &a);
ModesListRequest modesListRequestFromJson(const rapidjson::Value &v);

rapidjson::Value toJsonValue(const ModesGetRequest &r, rapidjson::Document::AllocatorType &a);
ModesGetRequest modesGetRequestFromJson(const rapidjson::Value &v);

rapidjson::Value toJsonValue(const AgentsSetModeRequest &r, rapidjson::Document::AllocatorType &a);
AgentsSetModeRequest agentsSetModeRequestFromJson(const rapidjson::Value &v);

rapidjson::Value toJsonValue(const PersonasListRequest &r, rapidjson::Document::AllocatorType &a);
PersonasListRequest personasListRequestFromJson(const rapidjson::Value &v);

rapidjson::Value toJsonValue(const ToolsCatalogRequest &r, rapidjson::Document::AllocatorType &a);
ToolsCatalogRequest toolsCatalogRequestFromJson(const rapidjson::Value &v);

rapidjson::Value toJsonValue(const BenchmarksListSupportedRequest &r, rapidjson::Document::AllocatorType &a);
BenchmarksListSupportedRequest benchmarksListSupportedRequestFromJson(const rapidjson::Value &v);

rapidjson::Value toJsonValue(const HooksRecentActivityRequest &r, rapidjson::Document::AllocatorType &a);
HooksRecentActivityRequest hooksRecentActivityRequestFromJson(const rapidjson::Value &v);

rapidjson::Value toJsonValue(const ModeSnapshot &r, rapidjson::Document::AllocatorType &a);
ModeSnapshot modeSnapshotFromJson(const rapidjson::Value &v);

rapidjson::Value toJsonValue(const ModeCatalogSnapshot &r, rapidjson::Document::AllocatorType &a);
ModeCatalogSnapshot modeCatalogSnapshotFromJson(const rapidjson::Value &v);

rapidjson::Value toJsonValue(const PersonaSnapshot &r, rapidjson::Document::AllocatorType &a);
PersonaSnapshot personaSnapshotFromJson(const rapidjson::Value &v);

rapidjson::Value toJsonValue(const PersonaCatalogSnapshot &r, rapidjson::Document::AllocatorType &a);
PersonaCatalogSnapshot personaCatalogSnapshotFromJson(const rapidjson::Value &v);

rapidjson::Value toJsonValue(const ToolSnapshot &r, rapidjson::Document::AllocatorType &a);
ToolSnapshot toolSnapshotFromJson(const rapidjson::Value &v);

rapidjson::Value toJsonValue(const ToolCatalogSnapshot &r, rapidjson::Document::AllocatorType &a);
ToolCatalogSnapshot toolCatalogSnapshotFromJson(const rapidjson::Value &v);

rapidjson::Value toJsonValue(const BenchmarkCatalogSnapshot &r, rapidjson::Document::AllocatorType &a);
BenchmarkCatalogSnapshot benchmarkCatalogSnapshotFromJson(const rapidjson::Value &v);

// ── /connect wizard ──────────────────────────────────────────────────────
rapidjson::Value toJsonValue(const WizardChoiceSnapshot &c, rapidjson::Document::AllocatorType &a);
WizardChoiceSnapshot wizardChoiceSnapshotFromJson(const rapidjson::Value &v);

rapidjson::Value toJsonValue(const WizardPromptSnapshot &p, rapidjson::Document::AllocatorType &a);
WizardPromptSnapshot wizardPromptSnapshotFromJson(const rapidjson::Value &v);

rapidjson::Value toJsonValue(const ConnectBeginRequest &r, rapidjson::Document::AllocatorType &a);
ConnectBeginRequest connectBeginRequestFromJson(const rapidjson::Value &v);

rapidjson::Value toJsonValue(const ConnectBeginResponse &r, rapidjson::Document::AllocatorType &a);
ConnectBeginResponse connectBeginResponseFromJson(const rapidjson::Value &v);

rapidjson::Value toJsonValue(const ConnectSubmitRequest &r, rapidjson::Document::AllocatorType &a);
ConnectSubmitRequest connectSubmitRequestFromJson(const rapidjson::Value &v);

rapidjson::Value toJsonValue(const ConnectSubmitResponse &r, rapidjson::Document::AllocatorType &a);
ConnectSubmitResponse connectSubmitResponseFromJson(const rapidjson::Value &v);

rapidjson::Value toJsonValue(const ConnectFinalizeRequest &r, rapidjson::Document::AllocatorType &a);
ConnectFinalizeRequest connectFinalizeRequestFromJson(const rapidjson::Value &v);

rapidjson::Value toJsonValue(const ConnectFinalizeResponse &r, rapidjson::Document::AllocatorType &a);
ConnectFinalizeResponse connectFinalizeResponseFromJson(const rapidjson::Value &v);

rapidjson::Value toJsonValue(const ConnectCancelRequest &r, rapidjson::Document::AllocatorType &a);
ConnectCancelRequest connectCancelRequestFromJson(const rapidjson::Value &v);

rapidjson::Value toJsonValue(const ConnectCancelResponse &r, rapidjson::Document::AllocatorType &a);
ConnectCancelResponse connectCancelResponseFromJson(const rapidjson::Value &v);

rapidjson::Value toJsonValue(const ConnectProgressSnapshot &s, rapidjson::Document::AllocatorType &a);
ConnectProgressSnapshot connectProgressSnapshotFromJson(const rapidjson::Value &v);

std::string connectProgressPhaseToWire(ConnectProgressPhase phase);
ConnectProgressPhase connectProgressPhaseFromWire(const std::string &str);

// ── /undo Rewind ──────────────────────────────────────────────────────────
std::string rewindModeToWire(RewindMode mode);
RewindMode rewindModeFromWire(const std::string &str);

rapidjson::Value toJsonValue(const RewindPreviewRequest &r, rapidjson::Document::AllocatorType &a);
RewindPreviewRequest rewindPreviewRequestFromJson(const rapidjson::Value &v);

rapidjson::Value toJsonValue(const RewindPreviewResponse &r, rapidjson::Document::AllocatorType &a);
RewindPreviewResponse rewindPreviewResponseFromJson(const rapidjson::Value &v);

rapidjson::Value toJsonValue(const RewindExecuteRequest &r, rapidjson::Document::AllocatorType &a);
RewindExecuteRequest rewindExecuteRequestFromJson(const rapidjson::Value &v);

rapidjson::Value toJsonValue(const RewindExecuteResponse &r, rapidjson::Document::AllocatorType &a);
RewindExecuteResponse rewindExecuteResponseFromJson(const rapidjson::Value &v);

rapidjson::Value toJsonValue(const RewindAppliedSnapshot &s, rapidjson::Document::AllocatorType &a);
RewindAppliedSnapshot rewindAppliedSnapshotFromJson(const rapidjson::Value &v);

// ── /redo ─────────────────────────────────────────────────────────────────
std::string redoModeToWire(RedoMode mode);
RedoMode redoModeFromWire(const std::string &str);

rapidjson::Value toJsonValue(const RedoUndoActionSummary &s, rapidjson::Document::AllocatorType &a);
RedoUndoActionSummary redoUndoActionSummaryFromJson(const rapidjson::Value &v);

rapidjson::Value toJsonValue(const RedoPreviewRequest &r, rapidjson::Document::AllocatorType &a);
RedoPreviewRequest redoPreviewRequestFromJson(const rapidjson::Value &v);

rapidjson::Value toJsonValue(const RedoPreviewResponse &r, rapidjson::Document::AllocatorType &a);
RedoPreviewResponse redoPreviewResponseFromJson(const rapidjson::Value &v);

rapidjson::Value toJsonValue(const RedoExecuteRequest &r, rapidjson::Document::AllocatorType &a);
RedoExecuteRequest redoExecuteRequestFromJson(const rapidjson::Value &v);

rapidjson::Value toJsonValue(const RedoExecuteResponse &r, rapidjson::Document::AllocatorType &a);
RedoExecuteResponse redoExecuteResponseFromJson(const rapidjson::Value &v);

} // namespace firmius::daemon

#endif // FIRMIUS_SERVER_PROTOCOLSERIALIZATION_HPP

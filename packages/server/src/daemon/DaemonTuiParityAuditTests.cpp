#include "daemon/DaemonClient.hpp"
#include "daemon/ProtocolSerialization.hpp"

#include <gtest/gtest.h>

#include <type_traits>

namespace firmius::daemon {
namespace {

template <typename T>
constexpr bool serializable_request_v = requires(
    const T &request, rapidjson::Document::AllocatorType &allocator) {
  { toJsonValue(request, allocator) } -> std::same_as<rapidjson::Value>;
};

template <typename T>
constexpr bool serializable_snapshot_v = requires(
    const T &snapshot, rapidjson::Document::AllocatorType &allocator,
    const rapidjson::Value &value) {
  { toJsonValue(snapshot, allocator) } -> std::same_as<rapidjson::Value>;
};

template <typename T>
constexpr bool has_from_json_v = requires(const rapidjson::Value &value) {
  { T::fromJson(value) } -> std::same_as<T>;
};

template <typename Client, typename Ret, typename... Args>
constexpr bool has_method(Ret (Client::*)(Args...) const) {
  return true;
}

template <typename Client, typename Ret, typename... Args>
constexpr bool has_method(Ret (Client::*)(Args...)) {
  return true;
}

template <typename Actual, typename Expected>
constexpr bool member_has_type(Actual Expected::*) {
  return std::is_same_v<Actual Expected::*, Actual Expected::*>;
}

TEST(DaemonTuiParityAudit, ClientFacadeCoversInProcessTuiBackendActions) {
  using Client = DaemonClient;

  static_assert(has_method<DaemonClient>(&Client::uiSnapshot));
  static_assert(has_method<DaemonClient>(&Client::session));
  static_assert(has_method<DaemonClient>(&Client::listClients));

  static_assert(has_method<DaemonClient>(
      static_cast<std::vector<firmius::shared::ThreadMetadata> (Client::*)() const>(
          &Client::listThreads)));
  static_assert(has_method<DaemonClient>(&Client::getThread));
  static_assert(has_method<DaemonClient>(
      static_cast<ThreadsOpenResponse (Client::*)(const ThreadsOpenRequest &) const>(
          &Client::openThread)));
  static_assert(has_method<DaemonClient>(&Client::focusThread));
  static_assert(has_method<DaemonClient>(&Client::createThread));
  static_assert(has_method<DaemonClient>(&Client::send));

  static_assert(has_method<DaemonClient>(&Client::listAgents));
  static_assert(has_method<DaemonClient>(&Client::getAgent));
  static_assert(has_method<DaemonClient>(&Client::focusAgent));
  static_assert(has_method<DaemonClient>(&Client::compactAgent));
  static_assert(has_method<DaemonClient>(&Client::interruptAgent));
  static_assert(has_method<DaemonClient>(&Client::setAgentMode));

  static_assert(has_method<DaemonClient>(&Client::listProcesses));
  static_assert(has_method<DaemonClient>(&Client::getProcess));
  static_assert(has_method<DaemonClient>(&Client::focusProcessState));

  static_assert(has_method<DaemonClient>(&Client::getTranscript));
  static_assert(has_method<DaemonClient>(&Client::listToolCalls));
  static_assert(has_method<DaemonClient>(&Client::subagentActivity));
  static_assert(has_method<DaemonClient>(&Client::listArtifacts));

  static_assert(has_method<DaemonClient>(&Client::listModels));
  static_assert(has_method<DaemonClient>(&Client::refreshModels));
  static_assert(has_method<DaemonClient>(&Client::switchModel));
  static_assert(has_method<DaemonClient>(&Client::listProviders));
  static_assert(has_method<DaemonClient>(&Client::updateProviderProfiles));
  static_assert(has_method<DaemonClient>(&Client::invalidateModelCache));
  static_assert(has_method<DaemonClient>(&Client::listAccounts));
  static_assert(has_method<DaemonClient>(&Client::deleteAccount));
  static_assert(has_method<DaemonClient>(&Client::getQuotas));
  static_assert(has_method<DaemonClient>(&Client::getCachedQuotas));

  static_assert(has_method<DaemonClient>(&Client::getPermissionMode));
  static_assert(has_method<DaemonClient>(&Client::setPermissionMode));
  static_assert(has_method<DaemonClient>(&Client::listPendingPermissions));
  static_assert(has_method<DaemonClient>(&Client::resolvePermission));

  static_assert(has_method<DaemonClient>(&Client::getConfig));
  static_assert(has_method<DaemonClient>(&Client::updateConfig));
  static_assert(has_method<DaemonClient>(&Client::getRouterConfig));
  static_assert(has_method<DaemonClient>(&Client::updateRouterConfig));
  static_assert(has_method<DaemonClient>(&Client::getPurposesConfig));
  static_assert(has_method<DaemonClient>(&Client::updatePurposesConfig));
  static_assert(has_method<DaemonClient>(&Client::getRollingMemoryConfig));
  static_assert(has_method<DaemonClient>(&Client::updateRollingMemoryConfig));
  static_assert(has_method<DaemonClient>(&Client::getMcpConfig));
  static_assert(has_method<DaemonClient>(&Client::updateMcpConfig));

  static_assert(has_method<DaemonClient>(&Client::getHistory));
  static_assert(has_method<DaemonClient>(&Client::undoHistory));
  static_assert(has_method<DaemonClient>(&Client::redoHistory));
  static_assert(has_method<DaemonClient>(&Client::undoTranscript));
  static_assert(has_method<DaemonClient>(&Client::redoTranscript));
  static_assert(has_method<DaemonClient>(&Client::listEdits));
  static_assert(has_method<DaemonClient>(&Client::undoEdit));
  static_assert(has_method<DaemonClient>(&Client::redoEdit));

  static_assert(has_method<DaemonClient>(&Client::listHooks));
  static_assert(has_method<DaemonClient>(&Client::reloadHooks));
  static_assert(has_method<DaemonClient>(&Client::hookState));
  static_assert(has_method<DaemonClient>(&Client::recentHookActivity));
  static_assert(has_method<DaemonClient>(&Client::listPacts));
  static_assert(has_method<DaemonClient>(&Client::getPact));
  static_assert(has_method<DaemonClient>(&Client::listWorkflows));
  static_assert(has_method<DaemonClient>(&Client::executeWorkflow));

  static_assert(has_method<DaemonClient>(&Client::listModes));
  static_assert(has_method<DaemonClient>(&Client::getMode));
  static_assert(has_method<DaemonClient>(&Client::listPersonas));
  static_assert(has_method<DaemonClient>(&Client::toolCatalog));
  static_assert(has_method<DaemonClient>(&Client::listSupportedBenchmarks));
  static_assert(has_method<DaemonClient>(&Client::startBenchmark));
  static_assert(has_method<DaemonClient>(&Client::getBenchmarkStatus));
  static_assert(has_method<DaemonClient>(&Client::getBenchmarkLogs));

  static_assert(has_method<DaemonClient>(&Client::subscribe));
  static_assert(has_method<DaemonClient>(&Client::unsubscribe));
  SUCCEED();
}

TEST(DaemonTuiParityAudit, UiSnapshotContainsAttachStateNeededByDetachedTui) {
  static_assert(member_has_type<ClientSessionSnapshot>(&UiSnapshot::session));
  static_assert(member_has_type<std::vector<firmius::shared::ThreadMetadata>>(&UiSnapshot::threads));
  static_assert(member_has_type<std::optional<ThreadSnapshot>>(&UiSnapshot::focusedThread));
  static_assert(member_has_type<AgentTreeSnapshot>(&UiSnapshot::agents));
  static_assert(member_has_type<std::optional<AgentRuntimeSnapshot>>(&UiSnapshot::focusedAgent));
  static_assert(member_has_type<std::optional<TranscriptSnapshot>>(&UiSnapshot::transcript));
  static_assert(member_has_type<std::vector<ToolCallSnapshot>>(&UiSnapshot::toolCalls));
  static_assert(member_has_type<SubagentActivitySnapshot>(&UiSnapshot::subagents));
  static_assert(member_has_type<ProcessRuntimeSummary>(&UiSnapshot::processSummary));
  static_assert(member_has_type<std::vector<ProcessSnapshot>>(&UiSnapshot::processes));
  static_assert(member_has_type<PermissionQueueSnapshot>(&UiSnapshot::permissions));
  static_assert(member_has_type<ModelCatalogSnapshot>(&UiSnapshot::models));
  static_assert(member_has_type<ProviderCatalogSnapshot>(&UiSnapshot::providers));
  static_assert(member_has_type<UserConfigSnapshot>(&UiSnapshot::config));
  static_assert(member_has_type<RouterConfigSnapshot>(&UiSnapshot::router));
  static_assert(member_has_type<PurposesConfigSnapshot>(&UiSnapshot::purposes));
  static_assert(member_has_type<RollingMemoryConfigSnapshot>(&UiSnapshot::rollingMemory));
  static_assert(member_has_type<McpConfigSnapshot>(&UiSnapshot::mcp));
  static_assert(member_has_type<HookStateSnapshot>(&UiSnapshot::hooks));
  static_assert(member_has_type<std::vector<PactSnapshot>>(&UiSnapshot::pacts));
  static_assert(member_has_type<ArtifactCatalogSnapshot>(&UiSnapshot::artifacts));
  static_assert(member_has_type<HistorySnapshot>(&UiSnapshot::history));
  static_assert(member_has_type<EditHistorySnapshot>(&UiSnapshot::edits));
  static_assert(member_has_type<std::uint64_t>(&UiSnapshot::latestEventSequence));
  SUCCEED();
}

TEST(DaemonTuiParityAudit, EventReplayProtocolCarriesCursorAndSequence) {
  static_assert(member_has_type<std::uint64_t>(&EventSubscriptionRequest::sinceSequence));
  static_assert(member_has_type<std::uint64_t>(&DaemonEventEnvelope::sequence));
  static_assert(member_has_type<std::uint64_t>(&UiSnapshot::latestEventSequence));
  SUCCEED();
}

TEST(DaemonTuiParityAudit, CriticalWireTypesAreSerializableForDetachedClient) {
  static_assert(serializable_request_v<UiSnapshotRequest>);
  static_assert(serializable_request_v<EventSubscriptionRequest>);
  static_assert(serializable_snapshot_v<UiSnapshot>);
  static_assert(serializable_snapshot_v<DaemonEventEnvelope>);
  static_assert(serializable_snapshot_v<ThreadSnapshot>);
  static_assert(serializable_snapshot_v<AgentRuntimeSnapshot>);
  static_assert(serializable_snapshot_v<AgentTreeSnapshot>);
  static_assert(serializable_snapshot_v<TranscriptSnapshot>);
  static_assert(serializable_snapshot_v<ToolCallSnapshot>);
  static_assert(serializable_snapshot_v<SubagentActivitySnapshot>);
  static_assert(serializable_snapshot_v<ProcessSnapshot>);
  static_assert(serializable_snapshot_v<ModelCatalogSnapshot>);
  static_assert(serializable_snapshot_v<ProviderCatalogSnapshot>);
  static_assert(serializable_snapshot_v<PermissionQueueSnapshot>);
  static_assert(serializable_snapshot_v<UserConfigSnapshot>);
  static_assert(serializable_snapshot_v<RouterConfigSnapshot>);
  static_assert(serializable_snapshot_v<PurposesConfigSnapshot>);
  static_assert(serializable_snapshot_v<RollingMemoryConfigSnapshot>);
  static_assert(serializable_snapshot_v<McpConfigSnapshot>);
  static_assert(serializable_snapshot_v<HookStateSnapshot>);
  static_assert(serializable_snapshot_v<HooksRecentActivitySnapshot>);
  static_assert(serializable_snapshot_v<PactSnapshot>);
  static_assert(serializable_snapshot_v<WorkflowExecutionSnapshot>);
  static_assert(serializable_snapshot_v<ArtifactCatalogSnapshot>);
  static_assert(serializable_snapshot_v<HistorySnapshot>);
  static_assert(serializable_snapshot_v<EditHistorySnapshot>);
  SUCCEED();
}

} // namespace
} // namespace firmius::daemon

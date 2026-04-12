#include "WebState.hpp"

#include "Serialization.hpp"
#include "AgentRegistry.hpp"
#include "agents/PurposeLoader.hpp"
#include "harness/Harness.hpp"
#include "persistence/ThreadManager.hpp"
#include "providers/BaseAPIKeyProvider.hpp"
#include "providers/BaseOAuthProvider.hpp"
#include "providers/ProviderRegistry.hpp"
#include "workflow/WorkflowLoader.hpp"

#include <json/value.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <system_error>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>
#include <unistd.h>

namespace firmius::web {
namespace {

using firmius::core::Harness;
using firmius::core::ThreadManager;
using firmius::shared::AgentContext;
using firmius::shared::AgentHistory;
using firmius::shared::AgentState;
using firmius::shared::AppEvent;
using firmius::shared::Message;
using firmius::shared::MessagePart;
using firmius::shared::ModelInfo;
using firmius::shared::ThreadMetadata;

Json::Value toJsonValue(const rapidjson::Document &doc) {
  rapidjson::StringBuffer buffer;
  rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
  doc.Accept(writer);
  Json::Value parsed;
  Json::CharReaderBuilder builder;
  std::string errs;
  std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
  reader->parse(buffer.GetString(), buffer.GetString() + buffer.GetSize(), &parsed,
                &errs);
  return parsed;
}

std::optional<std::string> compactionIdFromTurnId(const std::string &turnId) {
  constexpr const char *prefixes[] = {"compaction-start-", "compaction-summary-",
                                      "compaction-end-"};
  for (const char *prefix : prefixes) {
    const std::size_t len = std::char_traits<char>::length(prefix);
    if (turnId.rfind(prefix, 0) == 0) {
      return turnId.substr(len);
    }
  }
  return std::nullopt;
}

bool turnsEquivalentForTranscript(const firmius::shared::AgentTurn &lhs,
                                  const firmius::shared::AgentTurn &rhs) {
  return lhs.turnId == rhs.turnId;
}

bool isTranscriptMeaningfulMessage(const firmius::shared::Message &message) {
  if (message.role == firmius::shared::Role::User ||
      message.role == firmius::shared::Role::Assistant ||
      message.role == firmius::shared::Role::System) {
    for (const auto &part : message.content) {
      if (std::holds_alternative<firmius::shared::TextContent>(part) ||
          std::holds_alternative<firmius::shared::ThinkingContent>(part) ||
          std::holds_alternative<firmius::shared::ImageContent>(part) ||
          std::holds_alternative<firmius::shared::NoticeContent>(part) ||
          std::holds_alternative<firmius::shared::ErrorContent>(part)) {
        return true;
      }
    }
  }
  return false;
}

std::vector<firmius::shared::AgentTurn> filterSnapshotTurnsForTranscript(
    const std::vector<firmius::shared::AgentTurn> &turns) {
  std::vector<firmius::shared::AgentTurn> filtered;
  filtered.reserve(turns.size());
  for (const auto &turn : turns) {
    bool meaningful = false;
    for (const auto &message : turn.messages) {
      if (isTranscriptMeaningfulMessage(message)) {
        meaningful = true;
        break;
      }
    }
    if (meaningful) {
      filtered.push_back(turn);
    }
  }
  return filtered;
}

std::size_t overlappingSnapshotSuffixLength(
    const std::vector<firmius::shared::AgentTurn> &snapshotTurns,
    const std::vector<firmius::shared::AgentTurn> &currentTurns,
    std::size_t currentStart) {
  const std::size_t maxCount =
      std::min(snapshotTurns.size(), currentTurns.size() - currentStart);
  for (std::size_t count = maxCount; count > 0; --count) {
    bool allMatch = true;
    for (std::size_t i = 0; i < count; ++i) {
      if (!turnsEquivalentForTranscript(
              snapshotTurns[snapshotTurns.size() - count + i],
              currentTurns[currentStart + i])) {
        allMatch = false;
        break;
      }
    }
    if (allMatch) {
      return count;
    }
  }
  return 0;
}

std::vector<firmius::shared::AgentTurn> expandCompactionTurns(
    const std::vector<firmius::shared::AgentTurn> &turns,
    const std::unordered_map<std::string, firmius::core::CompactionSnapshot>
        &snapshots,
    std::unordered_set<std::string> &expandedIds) {
  std::vector<firmius::shared::AgentTurn> result;
  for (std::size_t i = 0; i < turns.size(); ++i) {
    const auto compactionId = compactionIdFromTurnId(turns[i].turnId);
    if (!compactionId.has_value() ||
        turns[i].turnId.rfind("compaction-start-", 0) != 0) {
      result.push_back(turns[i]);
      continue;
    }

    std::size_t blockEnd = i;
    while (blockEnd + 1 < turns.size()) {
      const auto nextId = compactionIdFromTurnId(turns[blockEnd + 1].turnId);
      if (!nextId.has_value() || *nextId != *compactionId) {
        break;
      }
      ++blockEnd;
      if (turns[blockEnd].turnId.rfind("compaction-end-", 0) == 0) {
        break;
      }
    }

    auto snapshotIt = snapshots.find(*compactionId);
    if (snapshotIt != snapshots.end() && !expandedIds.count(*compactionId)) {
      expandedIds.insert(*compactionId);
      const auto snapshotTurns =
          filterSnapshotTurnsForTranscript(snapshotIt->second.turns);
      const std::size_t overlap =
          overlappingSnapshotSuffixLength(snapshotTurns, turns, blockEnd + 1);
      auto expandedSnapshot =
          expandCompactionTurns(snapshotTurns, snapshots, expandedIds);
      result.insert(result.end(), expandedSnapshot.begin(), expandedSnapshot.end());
      for (std::size_t j = i; j <= blockEnd; ++j) {
        result.push_back(turns[j]);
      }
      i = blockEnd + overlap;
      if (i >= turns.size()) {
        break;
      }
      --i;
      continue;
    }

    for (std::size_t j = i; j <= blockEnd; ++j) {
      result.push_back(turns[j]);
    }
    i = blockEnd;
  }
  return result;
}

AgentHistory expandHistoryForTranscript(const std::string &threadId,
                                        const std::string &agentId,
                                        const AgentHistory &baseHistory) {
  if (threadId.empty() || agentId.empty()) {
    return baseHistory;
  }

  bool hasCompaction = false;
  for (const auto &turn : baseHistory.turns) {
    if (compactionIdFromTurnId(turn.turnId).has_value()) {
      hasCompaction = true;
      break;
    }
  }
  if (!hasCompaction) {
    return baseHistory;
  }

  ThreadManager tm(ThreadManager::defaultBasePath());
  const auto snapshotList = tm.loadCompactionSnapshots(threadId, agentId);
  if (snapshotList.empty()) {
    return baseHistory;
  }

  std::unordered_map<std::string, firmius::core::CompactionSnapshot> snapshots;
  for (const auto &snapshot : snapshotList) {
    if (!snapshot.compactionId.empty()) {
      snapshots[snapshot.compactionId] = snapshot;
    }
  }

  AgentHistory expanded = baseHistory;
  std::unordered_set<std::string> expandedIds;
  expanded.turns = expandCompactionTurns(baseHistory.turns, snapshots, expandedIds);
  return expanded;
}

AgentHistory loadExpandedAgentHistory(const std::string &threadId,
                                      const std::string &agentId) {
  if (threadId.empty() || agentId.empty()) {
    return {};
  }
  ThreadManager tm(ThreadManager::defaultBasePath());
  auto history = tm.loadAgentHistory(threadId, agentId);
  return expandHistoryForTranscript(threadId, agentId, history);
}

Json::Value serializeHistory(const AgentHistory &history) {
  Json::Value out(Json::objectValue);
  out["threadId"] = history.threadId;
  out["turns"] = Json::arrayValue;
  for (const auto &turn : history.turns) {
    out["turns"].append(toJsonValue(firmius::shared::toJson(turn)));
  }
  return out;
}

std::string permissionRequestTypeToString(
    firmius::shared::PermissionRequestType type) {
  switch (type) {
  case firmius::shared::PermissionRequestType::Command:
    return "Command";
  case firmius::shared::PermissionRequestType::Edit:
    return "Edit";
  }
  return "Command";
}

std::string permissionResponseToString(
    firmius::shared::PermissionResponse response) {
  switch (response) {
  case firmius::shared::PermissionResponse::AllowOnce:
    return "AllowOnce";
  case firmius::shared::PermissionResponse::AllowAlways:
    return "AllowAlways";
  case firmius::shared::PermissionResponse::Deny:
    return "Deny";
  }
  return "Deny";
}

std::string commandSeverityToString(firmius::shared::CommandSeverity severity) {
  switch (severity) {
  case firmius::shared::CommandSeverity::LOW:
    return "LOW";
  case firmius::shared::CommandSeverity::MEDIUM:
    return "MEDIUM";
  case firmius::shared::CommandSeverity::HIGH:
    return "HIGH";
  case firmius::shared::CommandSeverity::VULNERABLE:
    return "VULNERABLE";
  }
  return "LOW";
}

Json::Value serializePermissionRequest(
    const firmius::shared::PermissionEscalationRequest &request) {
  Json::Value out(Json::objectValue);
  out["requestId"] = request.requestId;
  out["threadId"] = request.threadId;
  out["agentId"] = request.agentId;
  out["requestType"] = permissionRequestTypeToString(request.requestType);
  out["title"] = request.title;
  out["message"] = request.message;
  out["command"] = request.command;
  out["severity"] = commandSeverityToString(request.severity);
  out["targetPath"] = request.targetPath;
  out["allowAlways"] = request.allowAlways;
  return out;
}

Json::Value serializePermissionRules(
    const firmius::core::ThreadPermissionRules &rules) {
  Json::Value out(Json::objectValue);
  out["commandAllowRules"] = Json::arrayValue;
  for (const auto &rule : rules.commandAllowRules) {
    Json::Value item(Json::objectValue);
    item["exactCommand"] = rule.exactCommand;
    item["normalizedCommand"] = rule.normalizedCommand;
    item["primaryCommand"] = rule.primaryCommand;
    item["severity"] = commandSeverityToString(rule.severity);
    out["commandAllowRules"].append(std::move(item));
  }
  out["writeAllowPaths"] = Json::arrayValue;
  for (const auto &path : rules.writeAllowPaths) {
    out["writeAllowPaths"].append(path);
  }
  return out;
}


Json::Value serializeLiveState(const firmius::core::AgentLiveState &state) {
  Json::Value out(Json::objectValue);
  out["threadId"] = state.threadId;
  out["agentId"] = state.agentId;
  return out;
}

Json::Value serializeAgentState(const AgentState &state) {
  Json::Value out(Json::objectValue);
  out["currentStatus"] = static_cast<int>(state.currentStatus);
  auto appendStrings = [&out](const char *key,
                              const std::vector<std::string> &values) {
    Json::Value array(Json::arrayValue);
    for (const auto &value : values) {
      array.append(value);
    }
    out[key] = std::move(array);
  };
  appendStrings("pendingToolCalls", state.pendingToolCalls);
  appendStrings("ownedProcesses", state.ownedProcesses);
  appendStrings("readFiles", state.readFiles);
  appendStrings("fullyReadFiles", state.fullyReadFiles);
  appendStrings("editedFiles", state.editedFiles);
  appendStrings("completedActions", state.completedActions);
  appendStrings("blockingProcessIds", state.blockingProcessIds);
  appendStrings("recentToolCallSignatures", state.recentToolCallSignatures);
  out["fatalError"] = state.fatalError.has_value() ? *state.fatalError : "";
  return out;
}

Json::Value serializeAgentInfo(const std::string &threadId,
                               const std::string &agentId,
                               const std::map<std::string, firmius::core::AgentManifestEntry> &manifest,
                               ThreadManager &tm) {
  Json::Value out(Json::objectValue);
  out["agentId"] = agentId;

  const auto it = manifest.find(agentId);
  if (it != manifest.end()) {
    out["persona"] = it->second.persona;
    out["parentId"] = it->second.parentId;
    out["friendlyName"] = it->second.friendlyName;
    out["title"] = it->second.title;
    out["persistHistory"] = it->second.persistHistory;
  }

  auto liveAgent = firmius::core::AgentRegistry::instance().getAgent(agentId);
  if (liveAgent && liveAgent->getContext().history &&
      liveAgent->getContext().history->threadId == threadId) {
    const AgentContext &ctx = liveAgent->getContext();
    out["identityName"] = ctx.identity.name;
    out["friendlyName"] = ctx.identity.friendlyName;
    out["title"] = ctx.identity.friendlyName.empty() ? ctx.identity.name
                                                      : ctx.identity.friendlyName;
    out["providerId"] = ctx.config.providerId;
    out["modelId"] = ctx.config.modelId;
    out["modelVariant"] = ctx.config.modelVariant;
    out["persona"] = ctx.config.personaName;
    out["state"] = serializeAgentState(ctx.state);
    out["isRunning"] = liveAgent->isRunning();
    out["isBooting"] = liveAgent->isBooting();
  } else {
    out["state"] = Json::objectValue;
    out["providerId"] = "";
    out["modelId"] = "";
    out["modelVariant"] = "";
    out["isRunning"] = false;
    out["isBooting"] = false;
  }

  out["liveState"] = serializeLiveState(tm.getAgentLiveState(threadId, agentId));
  return out;
}

std::string eventThreadId(const AppEvent &event) {
  return std::visit(
      [](const auto &ev) -> std::string {
        using T = std::decay_t<decltype(ev)>;
        if constexpr (std::is_same_v<T, firmius::shared::ThreadChanged>) {
          return ev.threadId;
        } else if constexpr (std::is_same_v<T, firmius::shared::ThreadDeleted>) {
          return ev.threadId;
        } else if constexpr (std::is_same_v<T, firmius::shared::ThreadLocked>) {
          return ev.threadId;
        } else if constexpr (std::is_same_v<T, firmius::shared::ThreadMetadataUpdated>) {
          return ev.threadId;
        } else if constexpr (requires { ev.threadId; }) {
          return ev.threadId;
        } else if constexpr (requires { ev.agentId; }) {
          auto agent = firmius::core::AgentRegistry::instance().getAgent(ev.agentId);
          if (agent && agent->getContext().history) {
            return agent->getContext().history->threadId;
          }
          return "";
        } else {
          return "";
        }
      },
      event);
}

Json::Value serializeAgentOutcome(const firmius::shared::AgentOutcome &outcome) {
  Json::Value out(Json::objectValue);
  switch (outcome.kind) {
  case firmius::shared::AgentOutcome::Kind::Response:
    out["kind"] = "Response";
    break;
  case firmius::shared::AgentOutcome::Kind::NoSummary:
    out["kind"] = "NoSummary";
    break;
  case firmius::shared::AgentOutcome::Kind::Cancelled:
    out["kind"] = "Cancelled";
    break;
  case firmius::shared::AgentOutcome::Kind::Failed:
    out["kind"] = "Failed";
    break;
  }
  out["text"] = outcome.text;
  out["artifactsCreated"] = Json::arrayValue;
  for (const auto &artifact : outcome.artifacts_created) {
    out["artifactsCreated"].append(toJsonValue(firmius::shared::toJson(artifact)));
  }
  out["artifactsUpdated"] = Json::arrayValue;
  for (const auto &artifact : outcome.artifacts_updated) {
    out["artifactsUpdated"].append(toJsonValue(firmius::shared::toJson(artifact)));
  }
  return out;
}

Json::Value serializeAppEvent(const AppEvent &event) {
  Json::Value out(Json::objectValue);
  std::visit(
      [&](const auto &ev) {
        using T = std::decay_t<decltype(ev)>;
        if constexpr (std::is_same_v<T, firmius::shared::AgentThinking>) {
          out["type"] = "agentThinking";
          out["agentId"] = ev.agentId;
          out["delta"] = ev.delta;
          out["parentId"] = ev.parentId;
        } else if constexpr (std::is_same_v<T, firmius::shared::AgentText>) {
          out["type"] = "agentText";
          out["agentId"] = ev.agentId;
          out["delta"] = ev.delta;
          out["parentId"] = ev.parentId;
        } else if constexpr (std::is_same_v<T, firmius::shared::AgentToolCallChunk>) {
          out["type"] = "agentToolCallChunk";
          out["agentId"] = ev.agentId;
          out["toolCallId"] = ev.toolCallId;
          out["nameDelta"] = ev.nameDelta;
          out["argsDelta"] = ev.argsDelta;
          out["index"] = ev.index;
          out["parentId"] = ev.parentId;
        } else if constexpr (std::is_same_v<T, firmius::shared::AgentToolCall>) {
          out["type"] = "agentToolCall";
          out["agentId"] = ev.agentId;
          out["toolCallId"] = ev.toolCallId;
          out["toolName"] = ev.toolName;
          out["toolArgs"] = ev.toolArgs;
          out["parentId"] = ev.parentId;
        } else if constexpr (std::is_same_v<T, firmius::shared::AgentTurnCompleted>) {
          out["type"] = "agentTurnCompleted";
          out["agentId"] = ev.agentId;
          out["turn"] = toJsonValue(firmius::shared::toJson(ev.turn));
          out["aggregateMetrics"] =
              toJsonValue(firmius::shared::toJson(ev.aggregateMetrics));
          out["parentId"] = ev.parentId;
        } else if constexpr (std::is_same_v<T, firmius::shared::AgentProviderWaiting>) {
          out["type"] = "agentProviderWaiting";
          out["agentId"] = ev.agentId;
          out["parentId"] = ev.parentId;
        } else if constexpr (std::is_same_v<T, firmius::shared::AgentRetrying>) {
          out["type"] = "agentRetrying";
          out["agentId"] = ev.agentId;
          out["attempt"] = ev.attempt;
          out["maxAttempts"] = ev.maxAttempts;
          out["httpStatus"] = ev.httpStatus;
          out["delayMs"] = ev.delayMs;
          out["reason"] = ev.reason;
          out["accountLocator"] = ev.accountLocator;
          out["parentId"] = ev.parentId;
        } else if constexpr (std::is_same_v<T, firmius::shared::AgentRetryFailed>) {
          out["type"] = "agentRetryFailed";
          out["agentId"] = ev.agentId;
          out["httpStatus"] = ev.httpStatus;
          out["reason"] = ev.reason;
          out["parentId"] = ev.parentId;
        } else if constexpr (std::is_same_v<T, firmius::shared::AgentAccountSwitched>) {
          out["type"] = "agentAccountSwitched";
          out["agentId"] = ev.agentId;
          out["accountLocator"] = ev.accountLocator;
          out["parentId"] = ev.parentId;
        } else if constexpr (std::is_same_v<T, firmius::shared::AgentCompacting>) {
          out["type"] = "agentCompacting";
          out["agentId"] = ev.agentId;
          out["parentId"] = ev.parentId;
        } else if constexpr (std::is_same_v<T, firmius::shared::AgentCompactionThinking>) {
          out["type"] = "agentCompactionThinking";
          out["agentId"] = ev.agentId;
          out["delta"] = ev.delta;
          out["parentId"] = ev.parentId;
        } else if constexpr (std::is_same_v<T, firmius::shared::AgentCompactionText>) {
          out["type"] = "agentCompactionText";
          out["agentId"] = ev.agentId;
          out["delta"] = ev.delta;
          out["parentId"] = ev.parentId;
        } else if constexpr (std::is_same_v<T, firmius::shared::ContextCompacted>) {
          out["type"] = "contextCompacted";
          out["agentId"] = ev.agentId;
          out["tokensSaved"] = ev.tokensSaved;
          out["parentId"] = ev.parentId;
        } else if constexpr (std::is_same_v<T, firmius::shared::AgentProcessSpawned>) {
          out["type"] = "agentProcessSpawned";
          out["agentId"] = ev.agentId;
          out["processId"] = ev.processId;
          out["toolCallId"] = ev.toolCallId;
          out["command"] = ev.command;
          out["parentId"] = ev.parentId;
        } else if constexpr (std::is_same_v<T, firmius::shared::AgentProcessOutput>) {
          out["type"] = "agentProcessOutput";
          out["agentId"] = ev.agentId;
          out["processId"] = ev.processId;
          out["output"] = ev.output;
          out["isStderr"] = ev.isStderr;
          out["finished"] = ev.finished;
          out["exitCode"] = ev.exitCode;
          out["durationMs"] = ev.durationMs;
          out["parentId"] = ev.parentId;
        } else if constexpr (std::is_same_v<T, firmius::shared::AgentSpawned>) {
          out["type"] = "agentSpawned";
          out["agentId"] = ev.agentId;
          out["parentId"] = ev.parentId;
          out["friendlyName"] = ev.friendlyName;
          out["title"] = ev.title;
          out["personaName"] = ev.personaName;
          out["providerId"] = ev.providerId;
          out["modelId"] = ev.modelId;
          out["persistHistory"] = ev.persistHistory;
          out["maxTokens"] = ev.maxTokens;
        } else if constexpr (std::is_same_v<T, firmius::shared::ThreadChanged>) {
          out["type"] = "threadChanged";
          out["threadId"] = ev.threadId;
          out["metadata"] = toJsonValue(firmius::shared::toJson(ev.metadata));
        } else if constexpr (std::is_same_v<T, firmius::shared::ThreadMetadataUpdated>) {
          out["type"] = "threadMetadataUpdated";
          out["threadId"] = ev.threadId;
          out["metadata"] = toJsonValue(firmius::shared::toJson(ev.metadata));
        } else if constexpr (std::is_same_v<T, firmius::shared::MessageQueued>) {
          out["type"] = "messageQueued";
          out["messageId"] = ev.messageId;
          out["text"] = ev.text;
          out["threadId"] = ev.threadId;
          out["agentId"] = ev.agentId;
        } else if constexpr (std::is_same_v<T, firmius::shared::MessageDequeued>) {
          out["type"] = "messageDequeued";
          out["messageId"] = ev.messageId;
          out["threadId"] = ev.threadId;
          out["agentId"] = ev.agentId;
        } else if constexpr (std::is_same_v<T, firmius::shared::UserMessageSent>) {
          out["type"] = "userMessageSent";
          out["messageId"] = ev.messageId;
          out["text"] = ev.text;
          out["threadId"] = ev.threadId;
        } else if constexpr (std::is_same_v<T, firmius::shared::AgentInterrupted>) {
          out["type"] = "agentInterrupted";
          out["agentId"] = ev.agentId;
          out["parentId"] = ev.parentId;
        } else if constexpr (std::is_same_v<T, firmius::shared::AgentError>) {
          out["type"] = "agentError";
          out["agentId"] = ev.agentId;
          out["message"] = ev.message;
          out["parentId"] = ev.parentId;
        } else if constexpr (std::is_same_v<T, firmius::shared::ModelSwitched>) {
          out["type"] = "modelSwitched";
          out["agentId"] = ev.agentId;
          out["oldProviderId"] = ev.oldProviderId;
          out["oldModelId"] = ev.oldModelId;
          out["newProviderId"] = ev.newProviderId;
          out["newModelId"] = ev.newModelId;
          out["parentId"] = ev.parentId;
        } else if constexpr (std::is_same_v<T, firmius::shared::HistoryUndone>) {
          out["type"] = "historyUndone";
          out["agentId"] = ev.agentId;
          out["threadId"] = ev.threadId;
          out["turnsRemoved"] = ev.turnsRemoved;
          out["compactionReversed"] = ev.compactionReversed;
          out["parentId"] = ev.parentId;
        } else if constexpr (std::is_same_v<T, firmius::shared::AgentFinished>) {
          out["type"] = "agentFinished";
          out["agentId"] = ev.agentId;
          out["outcome"] = serializeAgentOutcome(ev.outcome);
          out["parentId"] = ev.parentId;
        } else if constexpr (std::is_same_v<T, firmius::shared::PlanCreated>) {
          out["type"] = "planCreated";
          out["threadId"] = ev.threadId;
          out["plan"] = toJsonValue(firmius::shared::toJson(ev.plan));
        } else if constexpr (std::is_same_v<T, firmius::shared::PlanUpdated>) {
          out["type"] = "planUpdated";
          out["threadId"] = ev.threadId;
          out["plan"] = toJsonValue(firmius::shared::toJson(ev.plan));
        } else if constexpr (std::is_same_v<T, firmius::shared::PlanActivated>) {
          out["type"] = "planActivated";
          out["threadId"] = ev.threadId;
          out["planId"] = ev.planId;
          out["plan"] = toJsonValue(firmius::shared::toJson(ev.plan));
        } else if constexpr (std::is_same_v<T, firmius::shared::ChunkAdded>) {
          out["type"] = "chunkAdded";
          out["threadId"] = ev.threadId;
          out["planId"] = ev.planId;
          out["chunk"] = toJsonValue(firmius::shared::toJson(ev.chunk));
        } else if constexpr (std::is_same_v<T, firmius::shared::ChunkUpdated>) {
          out["type"] = "chunkUpdated";
          out["threadId"] = ev.threadId;
          out["planId"] = ev.planId;
          out["chunk"] = toJsonValue(firmius::shared::toJson(ev.chunk));
        } else if constexpr (std::is_same_v<T, firmius::shared::ChunkAssigned>) {
          out["type"] = "chunkAssigned";
          out["threadId"] = ev.threadId;
          out["planId"] = ev.planId;
          out["chunkId"] = ev.chunkId;
          out["assignedAgentId"] = ev.assignedAgentId;
          out["chunk"] = toJsonValue(firmius::shared::toJson(ev.chunk));
        } else if constexpr (std::is_same_v<T, firmius::shared::ChunkStatusChanged>) {
          out["type"] = "chunkStatusChanged";
          out["threadId"] = ev.threadId;
          out["planId"] = ev.planId;
          out["chunkId"] = ev.chunkId;
          out["oldStatus"] = static_cast<int>(ev.oldStatus);
          out["newStatus"] = static_cast<int>(ev.newStatus);
          out["chunk"] = toJsonValue(firmius::shared::toJson(ev.chunk));
        } else if constexpr (std::is_same_v<T,
                                             firmius::shared::PermissionEscalationRequest>) {
          out["type"] = "permissionEscalationRequest";
          out["request"] = serializePermissionRequest(ev);
        } else if constexpr (std::is_same_v<T,
                                             firmius::shared::PermissionEscalationResolved>) {
          out["type"] = "permissionEscalationResolved";
          out["requestId"] = ev.requestId;
          out["threadId"] = ev.threadId;
          out["agentId"] = ev.agentId;
          out["response"] = permissionResponseToString(ev.response);
        } else if constexpr (std::is_same_v<T, firmius::shared::ThreadLocked>) {
          out["type"] = "threadLocked";
          out["threadId"] = ev.threadId;
          out["ownerPid"] = ev.ownerPid;
        } else if constexpr (std::is_same_v<T, firmius::shared::ThreadDeleted>) {
          out["type"] = "threadDeleted";
          out["threadId"] = ev.threadId;
        } else if constexpr (std::is_same_v<T, firmius::shared::ConfigUpdated>) {
          out["type"] = "configUpdated";
        } else if constexpr (std::is_same_v<T, firmius::shared::ModelsRefreshed>) {
          out["type"] = "modelsRefreshed";
        } else if constexpr (std::is_same_v<T, firmius::shared::ThreadTitleUpdated>) {
          out["type"] = "threadTitleUpdated";
          out["threadId"] = ev.threadId;
          out["title"] = ev.title;
        } else {
          out["type"] = "other";
        }
      },
      event);
  out["threadId"] = eventThreadId(event);
  return out;
}

Json::Value serializeModel(const ModelInfo &model) {
  Json::Value out = toJsonValue(firmius::shared::toJson(model));
  if (!out.isMember("providerId") && out.isMember("provider")) {
    out["providerId"] = out["provider"];
  }
  return out;
}

Json::Value parseThemeFile(const std::filesystem::path &path) {
  std::ifstream file(path);
  if (!file.is_open()) {
    return Json::nullValue;
  }
  std::string content((std::istreambuf_iterator<char>(file)),
                      std::istreambuf_iterator<char>());
  Json::CharReaderBuilder builder;
  Json::Value parsed;
  std::string errs;
  std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
  if (!reader->parse(content.data(), content.data() + content.size(), &parsed,
                     &errs) ||
      !parsed.isObject()) {
    return Json::nullValue;
  }
  parsed["sourcePath"] = path.string();
  return parsed;
}

std::optional<std::string> loadLegacyThemeSelection() {
  const char *home = std::getenv("HOME");
  if (!home || !*home) {
    return std::nullopt;
  }

  const std::filesystem::path path =
      std::filesystem::path(home) / ".firmius/theme_selection.json";
  std::ifstream file(path);
  if (!file.is_open()) {
    return std::nullopt;
  }

  std::string content((std::istreambuf_iterator<char>(file)),
                      std::istreambuf_iterator<char>());
  if (content.empty()) {
    return std::nullopt;
  }

  Json::CharReaderBuilder builder;
  Json::Value parsed;
  std::string errs;
  std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
  if (!reader->parse(content.data(), content.data() + content.size(), &parsed,
                     &errs) ||
      !parsed.isObject() || !parsed.isMember("theme") ||
      !parsed["theme"].isString()) {
    return std::nullopt;
  }

  return parsed["theme"].asString();
}

std::optional<std::string> loadPreferredThemeName() {
  const char *home = std::getenv("HOME");
  if (!home || !*home) {
    return loadLegacyThemeSelection();
  }

  const std::filesystem::path path =
      std::filesystem::path(home) / ".firmius/preferences.json";
  std::ifstream file(path);
  if (!file.is_open()) {
    return loadLegacyThemeSelection();
  }

  std::string content((std::istreambuf_iterator<char>(file)),
                      std::istreambuf_iterator<char>());
  if (content.empty()) {
    return loadLegacyThemeSelection();
  }

  Json::CharReaderBuilder builder;
  Json::Value parsed;
  std::string errs;
  std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
  if (!reader->parse(content.data(), content.data() + content.size(), &parsed,
                     &errs) ||
      !parsed.isObject()) {
    return loadLegacyThemeSelection();
  }

  if (parsed.isMember("theme") && parsed["theme"].isString()) {
    return parsed["theme"].asString();
  }
  return loadLegacyThemeSelection();
}

std::string threadPermissionModeToString(
    firmius::shared::ThreadPermissionMode mode) {
  switch (mode) {
  case firmius::shared::ThreadPermissionMode::Request:
    return "Request";
  case firmius::shared::ThreadPermissionMode::AlwaysAllow:
    return "AlwaysAllow";
  case firmius::shared::ThreadPermissionMode::DenyAll:
    return "DenyAll";
  }
  return "Request";
}

std::string providerTypeToString(firmius::provider::ProviderType type) {
  switch (type) {
  case firmius::provider::ProviderType::OAuth:
    return "OAuth";
  case firmius::provider::ProviderType::APIKey:
    return "APIKey";
  }
  return "APIKey";
}

std::string workflowArgTypeToString(firmius::core::WorkflowArgType type) {
  switch (type) {
  case firmius::core::WorkflowArgType::String:
    return "String";
  case firmius::core::WorkflowArgType::Number:
    return "Number";
  case firmius::core::WorkflowArgType::Filepath:
    return "Filepath";
  case firmius::core::WorkflowArgType::AgentId:
    return "AgentId";
  case firmius::core::WorkflowArgType::ThreadId:
    return "ThreadId";
  }
  return "String";
}

Json::Value serializeModelRouteCategory(
    const firmius::shared::ModelRouteCategory &category) {
  Json::Value out(Json::objectValue);
  out["providerId"] = category.providerId;
  out["modelId"] = category.modelId;
  out["variantName"] = category.variantName;
  return out;
}

Json::Value serializeUserConfig(const firmius::shared::UserConfig &config) {
  Json::Value out(Json::objectValue);
  out["defaultProviderId"] = config.defaultProviderId;
  out["defaultModelId"] = config.defaultModelId;
  out["defaultModelVariant"] = config.defaultModelVariant;
  out["defaultLeadPersona"] = config.defaultLeadPersona;
  out["defaultTemperature"] = config.defaultTemperature;
  out["defaultMaxTokens"] = config.defaultMaxTokens.has_value()
                                ? Json::UInt(*config.defaultMaxTokens)
                                : Json::Value();
  out["dangerouslySkipPermissions"] = config.dangerouslySkipPermissions;
  out["showInternalNudges"] = config.showInternalNudges;

  out["apiKeys"] = Json::objectValue;
  for (const auto &[providerId, apiKey] : config.apiKeys) {
    out["apiKeys"][providerId] = apiKey;
  }

  out["providerOptions"] = Json::objectValue;
  for (const auto &[key, value] : config.providerOptions) {
    out["providerOptions"][key] = value;
  }

  out["modelRouterCategories"] = Json::objectValue;
  for (const auto &[name, category] : config.modelRouterCategories) {
    out["modelRouterCategories"][name] = serializeModelRouteCategory(category);
  }

  out["purposeRoutes"] = Json::objectValue;
  for (const auto &[purpose, category] : config.purposeRoutes) {
    out["purposeRoutes"][purpose] = category;
  }

  out["defaultRouteCategory"] = config.defaultRouteCategory;
  out["enableSubagentRouteFallback"] = config.enableSubagentRouteFallback;
  out["subagentRouteFallbackOrder"] = Json::arrayValue;
  for (const auto &name : config.subagentRouteFallbackOrder) {
    out["subagentRouteFallbackOrder"].append(name);
  }
  return out;
}

Json::Value serializeOAuthAccount(const firmius::shared::OAuthAccount &account) {
  Json::Value out(Json::objectValue);
  out["identifier"] = account.identifier;
  out["tokenExpiration"] = Json::Int64(account.tokenExpiration);
  out["lastQuotaRefresh"] = Json::Int64(account.lastQuotaRefresh);
  out["rateLimited"] = account.rateLimited;
  out["backoffUntil"] = Json::Int64(account.backoffUntil);
  out["hasRefreshToken"] = !account.refreshToken.empty();
  out["hasAccessToken"] = !account.accessToken.empty();
  out["metadata"] = Json::objectValue;
  for (const auto &[key, value] : account.metadata) {
    out["metadata"][key] = value;
  }
  return out;
}

Json::Value serializeQuotaBucket(const firmius::shared::QuotaBucket &bucket) {
  Json::Value out(Json::objectValue);
  out["name"] = bucket.name;
  out["remainingFraction"] = bucket.remainingFraction;
  out["resetTime"] = bucket.resetTime;
  out["note"] = bucket.note;
  return out;
}

Json::Value serializeWorkflow(const firmius::core::Workflow &workflow) {
  Json::Value out(Json::objectValue);
  out["id"] = workflow.id;
  out["name"] = workflow.name;
  out["description"] = workflow.description;
  out["body"] = workflow.body;
  out["argCount"] = Json::UInt64(workflow.argCount);
  out["args"] = Json::arrayValue;
  for (const auto &arg : workflow.args) {
    Json::Value item(Json::objectValue);
    item["name"] = arg.name;
    item["type"] = workflowArgTypeToString(arg.type);
    item["description"] = arg.description;
    item["optional"] = arg.optional;
    out["args"].append(std::move(item));
  }
  return out;
}

std::filesystem::path executablePath() {
  char buffer[4096];
  const ssize_t len = readlink("/proc/self/exe", buffer, sizeof(buffer) - 1);
  if (len <= 0) {
    return {};
  }
  buffer[len] = '\0';
  return std::filesystem::path(buffer);
}

std::vector<std::filesystem::path> candidateThemeRoots() {
  std::vector<std::filesystem::path> roots;
  if (const char *env = std::getenv("FIRMIUS_THEME_DIR"); env && *env) {
    roots.emplace_back(env);
  }

  const char *home = std::getenv("HOME");
  if (home && *home) {
    roots.emplace_back(std::filesystem::path(home) / ".firmius/themes");
  }

  const std::filesystem::path exe = executablePath();
  if (!exe.empty()) {
    const std::filesystem::path binDir = exe.parent_path();
    roots.push_back(binDir / "../share/firmius/themes");
    roots.push_back(binDir / "share/firmius/themes");
    roots.push_back(binDir / "themes");
  }

#ifdef FIRMIUS_WEB_THEMES_INSTALL_DIR
  roots.emplace_back(FIRMIUS_WEB_THEMES_INSTALL_DIR);
#endif
#ifdef FIRMIUS_SOURCE_THEMES_DIR
  roots.emplace_back(FIRMIUS_SOURCE_THEMES_DIR);
#endif

  return roots;
}

} // namespace

WebState &WebState::instance() {
  static WebState state;
  return state;
}

void WebState::init() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (initialized_) {
    return;
  }
  subscription_id_ =
      Harness::instance().subscribe([this](const AppEvent &event) { recordEvent(event); });
  initialized_ = true;
}

void WebState::shutdown() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!initialized_) {
    return;
  }
  Harness::instance().unsubscribe(subscription_id_);
  subscription_id_ = -1;
  initialized_ = false;
  events_.clear();
}

void WebState::recordEvent(const AppEvent &event) {
  LoggedEvent logged;
  logged.threadId = eventThreadId(event);
  logged.payload = serializeAppEvent(event);

  {
    std::lock_guard<std::mutex> lock(mutex_);
    logged.id = next_event_id_++;
    logged.payload["eventId"] = Json::UInt64(logged.id);
    events_.push_back(logged);
    constexpr size_t kMaxEvents = 4000;
    if (events_.size() > kMaxEvents) {
      events_.erase(events_.begin(),
                    events_.begin() + static_cast<long>(events_.size() - kMaxEvents));
    }
  }
  cv_.notify_all();
}

std::uint64_t WebState::latestEventId() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return next_event_id_ > 0 ? next_event_id_ - 1 : 0;
}

std::vector<WebState::LoggedEvent>
WebState::waitForEventsAfter(std::uint64_t afterId, const std::string &threadId,
                             int timeoutMs) {
  auto predicate = [&]() {
    return std::any_of(events_.begin(), events_.end(), [&](const LoggedEvent &event) {
      return event.id > afterId && (threadId.empty() || event.threadId.empty() ||
                                    event.threadId == threadId);
    });
  };

  std::unique_lock<std::mutex> lock(mutex_);
  cv_.wait_for(lock, std::chrono::milliseconds(timeoutMs), predicate);

  std::vector<LoggedEvent> result;
  for (const auto &event : events_) {
    if (event.id <= afterId) {
      continue;
    }
    if (!threadId.empty() && !event.threadId.empty() && event.threadId != threadId) {
      continue;
    }
    result.push_back(event);
  }
  return result;
}

Json::Value WebState::buildThreadsSnapshot() {
  auto &harness = Harness::instance();
  Json::Value payload(Json::objectValue);
  payload["currentThreadId"] = harness.currentThreadId();
  payload["focusedAgentId"] = harness.focusedAgentId();
  payload["threads"] = Json::arrayValue;
  for (const auto &thread : harness.listThreads()) {
    payload["threads"].append(toJsonValue(firmius::shared::toJson(thread)));
  }
  return payload;
}

Json::Value WebState::buildFocusedHistorySnapshot() {
  Json::Value payload(Json::objectValue);
  auto &harness = Harness::instance();
  const std::string threadId = harness.currentThreadId();
  const std::string agentId = harness.focusedAgentId();
  payload["threadId"] = threadId;
  payload["agentId"] = agentId;
  payload["history"] = serializeHistory(loadExpandedAgentHistory(threadId, agentId));
  return payload;
}

Json::Value WebState::buildThemesSnapshot() {
  Json::Value payload(Json::objectValue);
  payload["themes"] = Json::arrayValue;
  payload["preferredTheme"] = Json::nullValue;

  std::map<std::string, Json::Value> deduped;
  for (const auto &root : candidateThemeRoots()) {
    if (root.empty()) {
      continue;
    }
    std::error_code ec;
    if (!std::filesystem::exists(root, ec) || !std::filesystem::is_directory(root, ec)) {
      continue;
    }
    for (const auto &entry : std::filesystem::directory_iterator(root, ec)) {
      if (ec) {
        break;
      }
      if (!entry.is_regular_file() || entry.path().extension() != ".json") {
        continue;
      }
      Json::Value theme = parseThemeFile(entry.path());
      if (!theme.isObject()) {
        continue;
      }
      const std::string name = theme.isMember("name") && theme["name"].isString()
                                   ? theme["name"].asString()
                                   : entry.path().stem().string();
      deduped[name] = std::move(theme);
    }
  }

  for (auto &[_, theme] : deduped) {
    payload["themes"].append(std::move(theme));
  }
  if (auto preferred = loadPreferredThemeName()) {
    payload["preferredTheme"] = *preferred;
  }
  return payload;
}

Json::Value WebState::buildStateSnapshot() {
  auto &harness = Harness::instance();
  Json::Value payload(Json::objectValue);
  payload["currentThreadId"] = harness.currentThreadId();
  payload["focusedAgentId"] = harness.focusedAgentId();
  payload["latestEventId"] = Json::UInt64(latestEventId());
  payload["threads"] = buildThreadsSnapshot()["threads"];

  const std::string currentThreadId = harness.currentThreadId();
  if (!currentThreadId.empty()) {
    ThreadManager tm(ThreadManager::defaultBasePath());
    ThreadMetadata metadata = tm.getMetadata(currentThreadId);
    payload["thread"] = toJsonValue(firmius::shared::toJson(metadata));

    std::map<std::string, firmius::core::AgentManifestEntry> manifest =
        tm.readAgentManifest(currentThreadId);
    payload["agents"] = Json::arrayValue;
    auto agents = harness.listAgents(currentThreadId);
    for (const auto &agentId : agents) {
      payload["agents"].append(
          serializeAgentInfo(currentThreadId, agentId, manifest, tm));
    }
    payload["focusedHistory"] = serializeHistory(
        loadExpandedAgentHistory(currentThreadId, harness.focusedAgentId()));

    Json::Value permissions(Json::objectValue);
    permissions["mode"] =
        threadPermissionModeToString(metadata.permissionMode);
    permissions["rules"] =
        serializePermissionRules(harness.threadPermissionRules(currentThreadId));
    permissions["pending"] = Json::arrayValue;
    for (const auto &request :
         harness.listPendingPermissionEscalations(currentThreadId)) {
      permissions["pending"].append(serializePermissionRequest(request));
    }
    payload["permissions"] = std::move(permissions);
  } else {
    payload["thread"] = Json::nullValue;
    payload["agents"] = Json::arrayValue;
    payload["focusedHistory"] = Json::nullValue;
    payload["permissions"] = Json::objectValue;
  }

  payload["models"] = Json::arrayValue;
  for (const auto &model : harness.listAllModels()) {
    payload["models"].append(serializeModel(model));
  }
  Json::Value themesSnapshot = buildThemesSnapshot();
  payload["themes"] = themesSnapshot["themes"];
  payload["preferredTheme"] = themesSnapshot["preferredTheme"];

  return payload;
}

Json::Value WebState::buildConfigSnapshot() {
  auto &harness = Harness::instance();
  Json::Value payload(Json::objectValue);
  payload["config"] = serializeUserConfig(harness.getConfig());

  payload["purposes"] = Json::arrayValue;
  for (const auto &purpose : firmius::core::PurposeLoader::listPurposes()) {
    payload["purposes"].append(purpose);
  }

  payload["switchablePurposes"] = Json::arrayValue;
  for (const auto &purpose :
       firmius::core::PurposeLoader::listSwitchablePurposes()) {
    payload["switchablePurposes"].append(purpose);
  }
  return payload;
}

Json::Value WebState::buildProvidersSnapshot() {
  Json::Value payload(Json::objectValue);
  payload["providers"] = Json::arrayValue;

  auto &harness = Harness::instance();
  for (const auto &provider :
       firmius::provider::ProviderRegistry::instance().listProviders()) {
    if (!provider) {
      continue;
    }

    Json::Value item(Json::objectValue);
    item["id"] = provider->getId();
    item["type"] = providerTypeToString(provider->getProviderType());
    item["configured"] = provider->isConfigured();
    item["supportsConnectWizard"] = false;
    item["connectWizardType"] = Json::nullValue;
    if (std::dynamic_pointer_cast<firmius::provider::BaseOAuthProvider>(provider)) {
      item["supportsConnectWizard"] = true;
      item["connectWizardType"] = "OAuth";
    } else if (std::dynamic_pointer_cast<firmius::provider::BaseAPIKeyProvider>(
                   provider)) {
      item["supportsConnectWizard"] = true;
      item["connectWizardType"] = "APIKey";
    }
    item["models"] = Json::arrayValue;
    for (const auto &model : provider->listModels()) {
      item["models"].append(serializeModel(model));
    }

    item["accounts"] = Json::arrayValue;
    for (const auto &account : harness.getAccounts(provider->getId())) {
      item["accounts"].append(serializeOAuthAccount(account));
    }

    item["quotas"] = Json::objectValue;
    for (const auto &[accountId, buckets] : harness.getAllQuotas(provider->getId())) {
      Json::Value bucketArray(Json::arrayValue);
      for (const auto &bucket : buckets) {
        bucketArray.append(serializeQuotaBucket(bucket));
      }
      item["quotas"][accountId] = std::move(bucketArray);
    }

    payload["providers"].append(std::move(item));
  }
  return payload;
}

Json::Value WebState::buildWorkflowsSnapshot() {
  Json::Value payload(Json::objectValue);
  payload["workflows"] = Json::arrayValue;
  for (const auto &workflow :
       firmius::core::WorkflowLoader::instance().getAllWorkflows()) {
    payload["workflows"].append(serializeWorkflow(workflow));
  }
  return payload;
}

Json::Value WebState::buildWorkSnapshot() {
  Json::Value payload(Json::objectValue);
  payload["threadId"] = Harness::instance().currentThreadId();
  payload["activePlanId"] = "";
  payload["plans"] = Json::arrayValue;
  payload["todos"] = Json::arrayValue;
  payload["artifacts"] = Json::arrayValue;

  const std::string threadId = Harness::instance().currentThreadId();
  if (threadId.empty()) {
    payload["activePlan"] = Json::nullValue;
    return payload;
  }

  ThreadManager tm(ThreadManager::defaultBasePath());
  ThreadMetadata metadata = tm.getMetadata(threadId);
  payload["activePlanId"] = metadata.activePlanId;

  Json::Value activePlan = Json::nullValue;
  for (const auto &plan : tm.listPlans(threadId)) {
    Json::Value planValue = toJsonValue(firmius::shared::toJson(plan));
    payload["plans"].append(planValue);
    if (!metadata.activePlanId.empty() && plan.id == metadata.activePlanId) {
      activePlan = std::move(planValue);
    }
  }
  payload["activePlan"] = std::move(activePlan);

  for (const auto &agentId : Harness::instance().listAgents(threadId)) {
    payload["todos"].append(
        toJsonValue(firmius::shared::toJson(tm.getAgentTodo(threadId, agentId))));
  }

  for (const auto &artifact : Harness::instance().listArtifacts(threadId)) {
    payload["artifacts"].append(toJsonValue(firmius::shared::toJson(artifact)));
  }

  return payload;
}

} // namespace firmius::web

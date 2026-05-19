#include "daemon/DaemonService.hpp"
#include "daemon/DaemonLog.hpp"
#include "daemon/ProtocolSerialization.hpp"

#include "AgentRegistry.hpp"
#include "ConfigLoader.hpp"
#include "Engine.hpp"
#include "providers/ProviderRegistry.hpp"
#include "utils/Logger.hpp"
#include "workflow/WorkflowLoader.hpp"
#include "agents/hooks/HookRegistry.hpp"
#include "agents/hooks/HookState.hpp"
#include "harness/Harness.hpp"
#include "agents/modes/Mode.hpp"
#include "agents/PurposeLoader.hpp"
#include "tools/ToolRegistry.hpp"
#include "benchmarks/BenchmarkFactory.hpp"
#include "agents/hooks/HookRegistry.hpp"
#include "AgentRegistry.hpp"
#include "providers/BaseAPIKeyProvider.hpp"
#include "providers/BaseOAuthProvider.hpp"
#include "providers/ProviderRegistry.hpp"
#include "Serialization.hpp"
#include "utils/ToolSummaries.hpp"
#include "utils/ToolView.hpp"
#include "workflow/WorkflowLoader.hpp"

#include <variant>

#include <chrono>
#include <filesystem>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>
#include <sstream>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <algorithm>
#include <random>

#if defined(_WIN32)
#include <windows.h>
#else
#include <unistd.h>
#endif


namespace {

using firmius::shared::AppEvent;

std::string normalizePathForComparison(const std::string &path) {
  if (path.empty()) {
    return path;
  }
  std::error_code ec;
  std::filesystem::path p(path);
  if (!p.is_absolute()) {
    p = std::filesystem::absolute(p, ec);
  }
  if (!ec) {
    const auto canon = std::filesystem::weakly_canonical(p, ec);
    if (!ec) {
      return canon.string();
    }
  }
  return p.string();
}

std::string appEventTypeName(const firmius::shared::AppEvent &event) {
  return std::visit(
      [](const auto &e) -> std::string {
        using T = std::decay_t<decltype(e)>;
        if constexpr (std::is_same_v<T, firmius::shared::ThreadChanged>) {
          return "thread_changed";
        } else if constexpr (std::is_same_v<T, firmius::shared::UserMessageSent>) {
          return "user_message_sent";
        } else if constexpr (std::is_same_v<T, firmius::shared::AgentSpawned>) {
          return "agent_spawned";
        } else if constexpr (std::is_same_v<T, firmius::shared::AgentThinking>) {
          return "agent_thinking";
        } else if constexpr (std::is_same_v<T, firmius::shared::AgentText>) {
          return "agent_text";
        } else if constexpr (std::is_same_v<T, firmius::shared::AgentToolCall>) {
          return "agent_tool_call";
        } else if constexpr (std::is_same_v<T, firmius::shared::AgentTurnCompleted>) {
          return "agent_turn_completed";
        } else if constexpr (std::is_same_v<T, firmius::shared::AgentFinished>) {
          return "agent_finished";
        } else if constexpr (std::is_same_v<T, firmius::shared::AgentError>) {
          return "agent_error";
        } else if constexpr (std::is_same_v<T, firmius::shared::AgentInterrupted>) {
          return "agent_interrupted";
        } else if constexpr (std::is_same_v<T, firmius::shared::AgentProcessOutput>) {
          return "agent_process_output";
        } else if constexpr (std::is_same_v<T, firmius::shared::AgentProcessSpawned>) {
          return "agent_process_spawned";
        } else if constexpr (std::is_same_v<T, firmius::shared::AgentFileEdited>) {
          return "agent_file_edited";
        } else if constexpr (std::is_same_v<T, firmius::shared::MessageQueued>) {
          return "message_queued";
        } else if constexpr (std::is_same_v<T, firmius::shared::MessageDequeued>) {
          return "message_dequeued";
        } else if constexpr (std::is_same_v<T, firmius::shared::PermissionEscalationRequest>) {
          return "permission_escalation_request";
        } else if constexpr (std::is_same_v<T, firmius::shared::PermissionEscalationResolved>) {
          return "permission_escalation_resolved";
        } else if constexpr (std::is_same_v<T, firmius::shared::ThreadMetadataUpdated>) {
          return "thread_metadata_updated";
        } else if constexpr (std::is_same_v<T, firmius::shared::ModelsRefreshed>) {
          return "models_refreshed";
        } else if constexpr (std::is_same_v<T, firmius::shared::ModelSwitched>) {
          return "model_switched";
        } else if constexpr (std::is_same_v<T, firmius::shared::ConfigUpdated>) {
          return "config_updated";
        } else if constexpr (std::is_same_v<T, firmius::shared::AgentTodoUpdated>) {
          return "agent_todo_updated";
        } else if constexpr (std::is_same_v<T, firmius::shared::EmbeddingModelProgress>) {
          return "embedding_model_progress";
        } else {
          return "runtime_event";
        }
      },
      event);
}

template <typename T>
rapidjson::Document basicEventDocument(const char *typeName) {
  rapidjson::Document doc;
  doc.SetObject();
  doc.AddMember("type", rapidjson::Value(typeName, doc.GetAllocator()).Move(),
                doc.GetAllocator());
  return doc;
}

rapidjson::Value imagesToJson(const std::vector<firmius::shared::ImageContent> &images,
                              rapidjson::Document::AllocatorType &allocator) {
  rapidjson::Value out(rapidjson::kArrayType);
  for (const auto &image : images) {
    firmius::shared::MessagePart part = image;
    auto partDoc = firmius::shared::toJson(part);
    rapidjson::Value value(rapidjson::kObjectType);
    value.CopyFrom(partDoc, allocator);
    out.PushBack(value, allocator);
  }
  return out;
}

rapidjson::Document serializeAppEventDocument(const AppEvent &event) {
  return std::visit(
      [](const auto &e) -> rapidjson::Document {
        using T = std::decay_t<decltype(e)>;
        if constexpr (std::is_same_v<T, firmius::shared::AgentSpawned> ||
                      std::is_same_v<T, firmius::shared::AgentProviderWaiting> ||
                      std::is_same_v<T, firmius::shared::AgentRetrying> ||
                      std::is_same_v<T, firmius::shared::AgentRetryFailed> ||
                      std::is_same_v<T, firmius::shared::AgentThinking> ||
                      std::is_same_v<T, firmius::shared::AgentText> ||
                      std::is_same_v<T, firmius::shared::AgentToolCall> ||
                      std::is_same_v<T, firmius::shared::AgentToolCallChunk> ||
                      std::is_same_v<T, firmius::shared::AgentFileEdited> ||
                      std::is_same_v<T, firmius::shared::AgentMetricsStreamed> ||
                      std::is_same_v<T, firmius::shared::AgentInterrupted> ||
                      std::is_same_v<T, firmius::shared::AgentError> ||
                      std::is_same_v<T, firmius::shared::AgentCompacting> ||
                      std::is_same_v<T, firmius::shared::AgentCompactionThinking> ||
                      std::is_same_v<T, firmius::shared::AgentCompactionText> ||
                      std::is_same_v<T, firmius::shared::ContextCompacted> ||
                      std::is_same_v<T, firmius::shared::AgentProcessOutput> ||
                      std::is_same_v<T, firmius::shared::AgentProcessSpawned> ||
                      std::is_same_v<T, firmius::shared::ModelSwitched> ||
                      std::is_same_v<T, firmius::shared::HistoryUndone> ||
                      std::is_same_v<T, firmius::shared::AgentAccountSwitched>) {
          firmius::shared::EngineEvent engineEvent = e;
          return firmius::shared::toJson(engineEvent);
        } else if constexpr (std::is_same_v<T, firmius::shared::AgentTurnCompleted>) {
          auto doc = basicEventDocument<T>("AgentTurnCompleted");
          doc.AddMember("agentId",
                        rapidjson::Value(e.agentId.c_str(), doc.GetAllocator()).Move(),
                        doc.GetAllocator());
          doc.AddMember("parentId",
                        rapidjson::Value(e.parentId.c_str(), doc.GetAllocator()).Move(),
                        doc.GetAllocator());
          doc.AddMember("turnId",
                        rapidjson::Value(e.turn.turnId.c_str(), doc.GetAllocator()).Move(),
                        doc.GetAllocator());
          // Serialize the full turn data (including messages with tool results)
          // so tui-v2 can finalize tool call presentations.
          auto turnJson = firmius::shared::toJson(e.turn);
          rapidjson::Value turnValue(rapidjson::kObjectType);
          turnValue.CopyFrom(turnJson, doc.GetAllocator());
          doc.AddMember("turn", turnValue, doc.GetAllocator());
          auto metricsJson = firmius::shared::toJson(e.aggregateMetrics);
          rapidjson::Value metricsValue(rapidjson::kObjectType);
          metricsValue.CopyFrom(metricsJson, doc.GetAllocator());
          doc.AddMember("aggregateMetrics", metricsValue, doc.GetAllocator());
          return doc;
        } else if constexpr (std::is_same_v<T, firmius::shared::AgentFinished>) {
          auto doc = basicEventDocument<T>("AgentFinished");
          doc.AddMember("agentId",
                        rapidjson::Value(e.agentId.c_str(), doc.GetAllocator()).Move(),
                        doc.GetAllocator());
          doc.AddMember("parentId",
                        rapidjson::Value(e.parentId.c_str(), doc.GetAllocator()).Move(),
                        doc.GetAllocator());
          return doc;
        } else if constexpr (std::is_same_v<T, firmius::shared::ThreadChanged>) {
          auto doc = basicEventDocument<T>("ThreadChanged");
          doc.AddMember("threadId",
                        rapidjson::Value(e.threadId.c_str(), doc.GetAllocator()).Move(),
                        doc.GetAllocator());
          auto metadata = firmius::shared::toJson(e.metadata);
          rapidjson::Value metadataValue(rapidjson::kObjectType);
          metadataValue.CopyFrom(metadata, doc.GetAllocator());
          doc.AddMember("metadata", metadataValue, doc.GetAllocator());
          return doc;
        } else if constexpr (std::is_same_v<T, firmius::shared::ThreadMetadataUpdated>) {
          auto doc = basicEventDocument<T>("ThreadMetadataUpdated");
          doc.AddMember("threadId",
                        rapidjson::Value(e.threadId.c_str(), doc.GetAllocator()).Move(),
                        doc.GetAllocator());
          auto metadata = firmius::shared::toJson(e.metadata);
          rapidjson::Value metadataValue(rapidjson::kObjectType);
          metadataValue.CopyFrom(metadata, doc.GetAllocator());
          doc.AddMember("metadata", metadataValue, doc.GetAllocator());
          return doc;
        } else if constexpr (std::is_same_v<T, firmius::shared::UserMessageSent>) {
          auto doc = basicEventDocument<T>("UserMessageSent");
          doc.AddMember("messageId",
                        rapidjson::Value(e.messageId.c_str(), doc.GetAllocator()).Move(),
                        doc.GetAllocator());
          doc.AddMember("text",
                        rapidjson::Value(e.text.c_str(), doc.GetAllocator()).Move(),
                        doc.GetAllocator());
          doc.AddMember("threadId",
                        rapidjson::Value(e.threadId.c_str(), doc.GetAllocator()).Move(),
                        doc.GetAllocator());
          doc.AddMember("agentId",
                        rapidjson::Value(e.agentId.c_str(), doc.GetAllocator()).Move(),
                        doc.GetAllocator());
          doc.AddMember("images", imagesToJson(e.images, doc.GetAllocator()),
                        doc.GetAllocator());
          return doc;
        } else if constexpr (std::is_same_v<T, firmius::shared::AgentTodoUpdated>) {
          auto doc = basicEventDocument<T>("AgentTodoUpdated");
          doc.AddMember("threadId",
                        rapidjson::Value(e.threadId.c_str(), doc.GetAllocator()).Move(),
                        doc.GetAllocator());
          doc.AddMember("agentId",
                        rapidjson::Value(e.agentId.c_str(), doc.GetAllocator()).Move(),
                        doc.GetAllocator());
          auto todo = firmius::shared::toJson(e.todo);
          rapidjson::Value todoValue(rapidjson::kObjectType);
          todoValue.CopyFrom(todo, doc.GetAllocator());
          doc.AddMember("todo", todoValue, doc.GetAllocator());
          return doc;
        } else if constexpr (std::is_same_v<T, firmius::shared::MessageQueued>) {
          auto doc = basicEventDocument<T>("MessageQueued");
          doc.AddMember("messageId",
                        rapidjson::Value(e.messageId.c_str(), doc.GetAllocator()).Move(),
                        doc.GetAllocator());
          doc.AddMember("text",
                        rapidjson::Value(e.text.c_str(), doc.GetAllocator()).Move(),
                        doc.GetAllocator());
          doc.AddMember("threadId",
                        rapidjson::Value(e.threadId.c_str(), doc.GetAllocator()).Move(),
                        doc.GetAllocator());
          doc.AddMember("agentId",
                        rapidjson::Value(e.agentId.c_str(), doc.GetAllocator()).Move(),
                        doc.GetAllocator());
          doc.AddMember("images", imagesToJson(e.images, doc.GetAllocator()),
                        doc.GetAllocator());
          return doc;
        } else if constexpr (std::is_same_v<T, firmius::shared::MessageDequeued> ||
                             std::is_same_v<T, firmius::shared::InternalMessageDequeued>) {
          auto doc = basicEventDocument<T>(
              std::is_same_v<T, firmius::shared::MessageDequeued>
                  ? "MessageDequeued"
                  : "InternalMessageDequeued");
          doc.AddMember("messageId",
                        rapidjson::Value(e.messageId.c_str(), doc.GetAllocator()).Move(),
                        doc.GetAllocator());
          doc.AddMember("threadId",
                        rapidjson::Value(e.threadId.c_str(), doc.GetAllocator()).Move(),
                        doc.GetAllocator());
          doc.AddMember("agentId",
                        rapidjson::Value(e.agentId.c_str(), doc.GetAllocator()).Move(),
                        doc.GetAllocator());
          return doc;
        } else if constexpr (std::is_same_v<T, firmius::shared::InternalMessageQueued>) {
          auto doc = basicEventDocument<T>("InternalMessageQueued");
          doc.AddMember("messageId",
                        rapidjson::Value(e.messageId.c_str(), doc.GetAllocator()).Move(),
                        doc.GetAllocator());
          doc.AddMember("text",
                        rapidjson::Value(e.text.c_str(), doc.GetAllocator()).Move(),
                        doc.GetAllocator());
          doc.AddMember("threadId",
                        rapidjson::Value(e.threadId.c_str(), doc.GetAllocator()).Move(),
                        doc.GetAllocator());
          doc.AddMember("agentId",
                        rapidjson::Value(e.agentId.c_str(), doc.GetAllocator()).Move(),
                        doc.GetAllocator());
          return doc;
        } else if constexpr (std::is_same_v<T, firmius::shared::PermissionEscalationRequest>) {
          auto doc = basicEventDocument<T>("PermissionEscalationRequest");
          auto &alloc = doc.GetAllocator();
          doc.AddMember("requestId",
                        rapidjson::Value(e.requestId.c_str(), alloc).Move(),
                        alloc);
          doc.AddMember("threadId",
                        rapidjson::Value(e.threadId.c_str(), alloc).Move(),
                        alloc);
          doc.AddMember("agentId",
                        rapidjson::Value(e.agentId.c_str(), alloc).Move(),
                        alloc);
          doc.AddMember("title",
                        rapidjson::Value(e.title.c_str(), alloc).Move(),
                        alloc);
          doc.AddMember("message",
                        rapidjson::Value(e.message.c_str(), alloc).Move(),
                        alloc);
          doc.AddMember("toolName",
                        rapidjson::Value(e.toolName.c_str(), alloc).Move(),
                        alloc);
          doc.AddMember("toolCallId",
                        rapidjson::Value(e.toolCallId.c_str(), alloc).Move(),
                        alloc);
          doc.AddMember("command",
                        rapidjson::Value(e.command.c_str(), alloc).Move(),
                        alloc);
          doc.AddMember("commandPrimary",
                        rapidjson::Value(e.commandPrimary.c_str(), alloc).Move(),
                        alloc);
          doc.AddMember("targetPath",
                        rapidjson::Value(e.targetPath.c_str(), alloc).Move(),
                        alloc);
          doc.AddMember("severity",
                        static_cast<int>(e.severity), alloc);
          doc.AddMember("requestType",
                        static_cast<int>(e.requestType), alloc);
          doc.AddMember("allowAlways", e.allowAlways, alloc);
          doc.AddMember("isDirectory", e.isDirectory, alloc);
          // ── v2 fields ──
          doc.AddMember("category",
                        rapidjson::Value(e.category.c_str(), alloc).Move(),
                        alloc);
          doc.AddMember("cwd",
                        rapidjson::Value(e.cwd.c_str(), alloc).Move(), alloc);
          doc.AddMember("url",
                        rapidjson::Value(e.url.c_str(), alloc).Move(), alloc);
          doc.AddMember("host",
                        rapidjson::Value(e.host.c_str(), alloc).Move(), alloc);
          doc.AddMember("scheme",
                        rapidjson::Value(e.scheme.c_str(), alloc).Move(), alloc);
          doc.AddMember("query",
                        rapidjson::Value(e.query.c_str(), alloc).Move(), alloc);
          doc.AddMember("persona",
                        rapidjson::Value(e.persona.c_str(), alloc).Move(), alloc);
          doc.AddMember("parentPersona",
                        rapidjson::Value(e.parentPersona.c_str(), alloc).Move(),
                        alloc);

          rapidjson::Value subs(rapidjson::kArrayType);
          for (const auto &s : e.subcommands) {
            subs.PushBack(rapidjson::Value(s.c_str(), alloc).Move(), alloc);
          }
          doc.AddMember("subcommands", subs, alloc);

          rapidjson::Value scopes(rapidjson::kArrayType);
          for (const auto &s : e.toolScopes) {
            scopes.PushBack(rapidjson::Value(s.c_str(), alloc).Move(), alloc);
          }
          doc.AddMember("toolScopes", scopes, alloc);

          rapidjson::Value sugg(rapidjson::kArrayType);
          for (const auto &s : e.suggestions) {
            rapidjson::Value v(rapidjson::kObjectType);
            v.AddMember("label",
                        rapidjson::Value(s.label.c_str(), alloc).Move(), alloc);
            v.AddMember("explanation",
                        rapidjson::Value(s.explanation.c_str(), alloc).Move(),
                        alloc);
            v.AddMember("ruleId",
                        rapidjson::Value(s.ruleId.c_str(), alloc).Move(), alloc);
            v.AddMember("category",
                        rapidjson::Value(s.category.c_str(), alloc).Move(),
                        alloc);
            v.AddMember("decision",
                        rapidjson::Value(s.decision.c_str(), alloc).Move(),
                        alloc);
            v.AddMember("scope",
                        rapidjson::Value(s.scope.c_str(), alloc).Move(), alloc);
            v.AddMember("comment",
                        rapidjson::Value(s.comment.c_str(), alloc).Move(),
                        alloc);
            v.AddMember("defaultSelected", s.defaultSelected, alloc);
            rapidjson::Value match(rapidjson::kObjectType);
            for (const auto &[k, val] : s.match) {
              match.AddMember(rapidjson::Value(k.c_str(), alloc).Move(),
                              rapidjson::Value(val.c_str(), alloc).Move(),
                              alloc);
            }
            v.AddMember("match", match, alloc);
            sugg.PushBack(v, alloc);
          }
          doc.AddMember("suggestions", sugg, alloc);
          return doc;
        } else if constexpr (std::is_same_v<T, firmius::shared::PermissionEscalationResolved>) {
          auto doc = basicEventDocument<T>("PermissionEscalationResolved");
          doc.AddMember("requestId",
                        rapidjson::Value(e.requestId.c_str(), doc.GetAllocator()).Move(),
                        doc.GetAllocator());
          doc.AddMember("threadId",
                        rapidjson::Value(e.threadId.c_str(), doc.GetAllocator()).Move(),
                        doc.GetAllocator());
          doc.AddMember("agentId",
                        rapidjson::Value(e.agentId.c_str(), doc.GetAllocator()).Move(),
                        doc.GetAllocator());
          doc.AddMember("response", static_cast<int>(e.response), doc.GetAllocator());
          return doc;
        } else if constexpr (std::is_same_v<T, firmius::shared::ModelsRefreshed>) {
          return basicEventDocument<T>("ModelsRefreshed");
        } else if constexpr (std::is_same_v<T, firmius::shared::EmbeddingModelProgress>) {
          auto doc = basicEventDocument<T>("EmbeddingModelProgress");
          auto &alloc = doc.GetAllocator();
          doc.AddMember("agentId", rapidjson::Value(e.agentId.c_str(), alloc), alloc);
          doc.AddMember("parentId", rapidjson::Value(e.parentId.c_str(), alloc), alloc);
          doc.AddMember("modelId", rapidjson::Value(e.modelId.c_str(), alloc), alloc);
          doc.AddMember("bytesDownloaded", e.bytesDownloaded, alloc);
          doc.AddMember("totalBytes", e.totalBytes, alloc);
          doc.AddMember("status", rapidjson::Value(e.status.c_str(), alloc), alloc);
          return doc;
        } else {
          return basicEventDocument<T>(typeid(T).name());
        }
      },
      event);
}

std::string serializeAppEvent(const AppEvent &event) {
  auto doc = serializeAppEventDocument(event);
  rapidjson::StringBuffer buffer;
  rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
  doc.Accept(writer);
  return buffer.GetString();
}

std::string eventThreadIdForRouting(const AppEvent &event) {
  const std::string directThreadId = std::visit(
      [](const auto &e) -> std::string {
        if constexpr (requires { e.threadId; }) {
          return e.threadId;
        }
        return "";
      },
      event);
  if (!directThreadId.empty()) {
    return directThreadId;
  }
  const std::string agentId = std::visit(
      [](const auto &e) -> std::string {
        if constexpr (requires { e.agentId; }) {
          return e.agentId;
        }
        return "";
      },
      event);
  if (agentId.empty()) {
    return "";
  }
  auto agent = firmius::core::AgentRegistry::instance().getAgent(agentId);
  if (!agent || !agent->getContext().history) {
    return "";
  }
  return agent->getContext().history->threadId;
}

std::string appEventAgentId(const AppEvent &event) {
  return std::visit(
      [](const auto &e) -> std::string {
        if constexpr (requires { e.agentId; }) {
          return e.agentId;
        }
        return "";
      },
      event);
}

bool subscriptionWantsEvent(const std::unordered_set<std::string> &eventKinds,
                            const std::string &eventKind) {
  return eventKinds.empty() || eventKinds.count(eventKind) > 0;
}

std::optional<std::string>
compactionIdFromTurnIdForDaemonDisplay(const std::string &turnId) {
  constexpr const char *prefixes[] = {"compaction-start-",
                                      "compaction-summary-",
                                      "compaction-end-"};
  for (const char *prefix : prefixes) {
    const std::size_t len = std::char_traits<char>::length(prefix);
    if (turnId.rfind(prefix, 0) == 0) {
      return turnId.substr(len);
    }
  }
  return std::nullopt;
}

std::size_t overlappingSnapshotSuffixLengthForDaemonDisplay(
    const std::vector<firmius::shared::AgentTurn> &snapshotTurns,
    const std::vector<firmius::shared::AgentTurn> &currentTurns,
    std::size_t currentStart) {
  if (currentTurns.size() <= currentStart) {
    return 0;
  }
  const std::size_t maxCount =
      std::min(snapshotTurns.size(), currentTurns.size() - currentStart);
  for (std::size_t count = maxCount; count > 0; --count) {
    bool allMatch = true;
    for (std::size_t i = 0; i < count; ++i) {
      if (snapshotTurns[snapshotTurns.size() - count + i].turnId !=
          currentTurns[currentStart + i].turnId) {
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

std::size_t overlappingRenderedPrefixLengthForDaemonDisplay(
    const std::vector<firmius::shared::AgentTurn> &renderedTurns,
    const std::vector<firmius::shared::AgentTurn> &snapshotTurns) {
  const std::size_t maxCount =
      std::min(renderedTurns.size(), snapshotTurns.size());
  for (std::size_t count = maxCount; count > 0; --count) {
    bool allMatch = true;
    for (std::size_t i = 0; i < count; ++i) {
      if (renderedTurns[renderedTurns.size() - count + i].turnId !=
          snapshotTurns[i].turnId) {
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

std::vector<firmius::shared::AgentTurn> expandCompactionTranscriptTurnsForDaemon(
    const std::vector<firmius::shared::AgentTurn> &turns,
    const std::unordered_map<std::string, firmius::core::CompactionSnapshot>
        &snapshots,
    std::unordered_set<std::string> &expandedIds) {
  std::vector<firmius::shared::AgentTurn> result;
  for (std::size_t i = 0; i < turns.size(); ++i) {
    const auto compactionId =
        compactionIdFromTurnIdForDaemonDisplay(turns[i].turnId);
    if (!compactionId.has_value() ||
        turns[i].turnId.rfind("compaction-start-", 0) != 0) {
      result.push_back(turns[i]);
      continue;
    }

    std::size_t blockEnd = i;
    while (blockEnd + 1 < turns.size()) {
      const auto nextId =
          compactionIdFromTurnIdForDaemonDisplay(turns[blockEnd + 1].turnId);
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
      auto expandedSnapshot = expandCompactionTranscriptTurnsForDaemon(
          snapshotIt->second.turns, snapshots, expandedIds);
      const std::size_t renderedOverlap =
          overlappingRenderedPrefixLengthForDaemonDisplay(result, expandedSnapshot);
      if (renderedOverlap > 0 && renderedOverlap <= expandedSnapshot.size()) {
        expandedSnapshot.erase(expandedSnapshot.begin(),
                               expandedSnapshot.begin() + renderedOverlap);
      }
      result.insert(result.end(), expandedSnapshot.begin(),
                    expandedSnapshot.end());
      for (std::size_t j = i; j <= blockEnd; ++j) {
        result.push_back(turns[j]);
      }
      const std::size_t overlap = overlappingSnapshotSuffixLengthForDaemonDisplay(
          snapshotIt->second.turns, turns, blockEnd + 1);
      const std::size_t nextIndex = blockEnd + overlap + 1;
      if (nextIndex >= turns.size()) {
        break;
      }
      i = nextIndex - 1;
      continue;
    }

    for (std::size_t j = i; j <= blockEnd; ++j) {
      result.push_back(turns[j]);
    }
    i = blockEnd;
  }
  return result;
}

std::vector<firmius::shared::AgentTurn> expandCompactionTranscriptForDaemon(
    const std::vector<firmius::shared::AgentTurn> &turns,
    const std::unordered_map<std::string, firmius::core::CompactionSnapshot>
        &snapshots) {
  std::unordered_set<std::string> expandedIds;
  return expandCompactionTranscriptTurnsForDaemon(turns, snapshots, expandedIds);
}

struct ParsedSubagentArgs {
  std::string action;
  std::string agentId;
  std::string name;
  std::string title;
  std::string task;
  std::string category;
};

struct ParsedSubagentResult {
  std::string agentId;
  std::string status;
  std::string result;
  std::string error;
  bool fallbackUsed = false;
  std::string routeCategory;
  std::vector<std::string> attemptedCategories;
  std::vector<std::string> artifactsCreated;
  std::vector<std::string> artifactsUpdated;
};

bool isDelegateLikeTool(const std::string &toolName) {
  return toolName == "Delegate" || toolName == "summon_subagent" ||
         toolName == "subagent_wait" || toolName == "terminate_subagent" ||
         toolName == "subagent_terminate";
}

bool isWaitLikeDelegateTool(const std::string &toolName,
                            const ParsedSubagentArgs &args) {
  if (toolName == "subagent_wait") {
    return true;
  }
  return toolName == "Delegate" && args.action == "Wait";
}

ParsedSubagentArgs parseSubagentArgsForDaemon(const std::string &argsJson) {
  ParsedSubagentArgs parsed;
  rapidjson::Document doc;
  doc.Parse(argsJson.c_str());
  if (doc.HasParseError() || !doc.IsObject()) {
    return parsed;
  }
  if (doc.HasMember("action") && doc["action"].IsString()) {
    parsed.action = doc["action"].GetString();
  }
  if (doc.HasMember("agent_id") && doc["agent_id"].IsString()) {
    parsed.agentId = doc["agent_id"].GetString();
  }
  if (doc.HasMember("name") && doc["name"].IsString()) {
    parsed.name = doc["name"].GetString();
  }
  if (doc.HasMember("title") && doc["title"].IsString()) {
    parsed.title = doc["title"].GetString();
  }
  if (doc.HasMember("task") && doc["task"].IsString()) {
    parsed.task = doc["task"].GetString();
  }
  if (doc.HasMember("category") && doc["category"].IsString()) {
    parsed.category = doc["category"].GetString();
  }
  return parsed;
}

ParsedSubagentResult parseSubagentResultForDaemon(const std::string &resultJson) {
  ParsedSubagentResult parsed;
  rapidjson::Document doc;
  doc.Parse(resultJson.c_str());
  if (doc.HasParseError() || !doc.IsObject()) {
    return parsed;
  }
  if (doc.HasMember("agentId") && doc["agentId"].IsString()) {
    parsed.agentId = doc["agentId"].GetString();
  } else if (doc.HasMember("agent_id") && doc["agent_id"].IsString()) {
    parsed.agentId = doc["agent_id"].GetString();
  }
  if (doc.HasMember("status") && doc["status"].IsString()) {
    parsed.status = doc["status"].GetString();
  }
  if (doc.HasMember("result") && doc["result"].IsString()) {
    parsed.result = doc["result"].GetString();
  }
  if (doc.HasMember("error") && doc["error"].IsString()) {
    parsed.error = doc["error"].GetString();
  }
  if (doc.HasMember("fallback_used") && doc["fallback_used"].IsBool()) {
    parsed.fallbackUsed = doc["fallback_used"].GetBool();
  }
  if (doc.HasMember("category") && doc["category"].IsString()) {
    parsed.routeCategory = doc["category"].GetString();
  }
  if (doc.HasMember("attempted_categories") &&
      doc["attempted_categories"].IsArray()) {
    for (const auto &entry : doc["attempted_categories"].GetArray()) {
      if (entry.IsString()) {
        parsed.attemptedCategories.push_back(entry.GetString());
      }
    }
  }
  auto parseArtifactArray = [](const rapidjson::Value &value) {
    std::vector<std::string> refs;
    if (!value.IsArray()) {
      return refs;
    }
    for (const auto &item : value.GetArray()) {
      if (item.IsString()) {
        refs.push_back(item.GetString());
      } else if (item.IsObject() && item.HasMember("reference") &&
                 item["reference"].IsString()) {
        refs.push_back(item["reference"].GetString());
      } else if (item.IsObject() && item.HasMember("filename") &&
                 item["filename"].IsString()) {
        refs.push_back(item["filename"].GetString());
      }
    }
    return refs;
  };
  if (doc.HasMember("artifacts_created")) {
    parsed.artifactsCreated = parseArtifactArray(doc["artifacts_created"]);
  }
  if (doc.HasMember("artifacts_updated")) {
    parsed.artifactsUpdated = parseArtifactArray(doc["artifacts_updated"]);
  }
  return parsed;
}

std::string extractProcessIdForDaemon(const std::string &resultJson) {
  rapidjson::Document doc;
  doc.Parse(resultJson.c_str());
  if (doc.HasParseError() || !doc.IsObject()) {
    return "";
  }
  if (doc.HasMember("process_id") && doc["process_id"].IsString()) {
    return doc["process_id"].GetString();
  }
  if (doc.HasMember("processId") && doc["processId"].IsString()) {
    return doc["processId"].GetString();
  }
  return "";
}

std::string summarizeHistoricalToolEntryForDaemon(const std::string &name,
                                                  const std::string &args,
                                                  const std::string &result,
                                                  bool success) {
  (void)result;
  return firmius::shared::SummarizeToolCall(
      name, args,
      success ? firmius::shared::ToolPhase::Finished
              : firmius::shared::ToolPhase::Error);
}

std::string subagentOutcomeStringFromWaitState(const std::string &waitState) {
  if (waitState == "completed") {
    return "response";
  }
  if (waitState == "completed_no_summary") {
    return "no_summary";
  }
  if (waitState == "cancelled") {
    return "cancelled";
  }
  if (waitState == "failed") {
    return "failed";
  }
  return "unknown";
}

std::string stringifyRapidJsonValue(const rapidjson::Value &value) {
  rapidjson::StringBuffer buffer;
  rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
  value.Accept(writer);
  return buffer.GetString();
}

std::vector<firmius::daemon::SubagentActivityLogEntrySnapshot>
synthesizeHistoricalSubagentLogForDaemon(
    const firmius::shared::AgentHistory &history, const std::string &task,
    const std::string &terminalWaitState, const std::string &errorText) {
  std::vector<firmius::daemon::SubagentActivityLogEntrySnapshot> entries;
  std::unordered_map<std::string, std::size_t> toolIndexById;
  if (!task.empty()) {
    entries.push_back({"Task: " + task, "finished", "", "", ""});
  }
  for (const auto &turn : history.turns) {
    for (const auto &msg : turn.messages) {
      for (const auto &content : msg.content) {
        if (const auto *toolCall =
                std::get_if<firmius::shared::ToolCallContent>(&content)) {
          firmius::daemon::SubagentActivityLogEntrySnapshot entry;
          entry.summary = firmius::shared::SummarizeToolCall(
              toolCall->name, toolCall->args, firmius::shared::ToolPhase::Finished);
          entry.phase = "finished";
          entry.toolCallId = toolCall->id;
          entry.toolName = toolCall->name;
          entry.toolArgsJson = toolCall->args;
          toolIndexById[toolCall->id] = entries.size();
          entries.push_back(std::move(entry));
        } else if (const auto *toolResult =
                       std::get_if<firmius::shared::ToolResultContent>(&content)) {
          auto it = toolIndexById.find(toolResult->toolCallId);
          if (it == toolIndexById.end()) {
            continue;
          }
          auto &entry = entries[it->second];
          entry.phase = toolResult->success ? "finished" : "error";
          entry.summary = summarizeHistoricalToolEntryForDaemon(
              entry.toolName, entry.toolArgsJson, toolResult->result,
              toolResult->success);
        } else if (const auto *thinking =
                       std::get_if<firmius::shared::ThinkingContent>(&content)) {
          if (!thinking->thinking.empty()) {
            entries.push_back({"Thought", "finished", "", "", ""});
          }
        }
      }
    }
  }
  if (terminalWaitState == "completed_no_summary") {
    entries.push_back({"Done (no summary)", "finished", "", "", ""});
  } else if (terminalWaitState == "cancelled") {
    entries.push_back({"Cancelled", "finished", "", "", ""});
  } else if (terminalWaitState == "failed") {
    entries.push_back({errorText.empty() ? "Failed" : "Failed: " + errorText,
                       "error", "", "", ""});
  } else if (terminalWaitState == "completed") {
    entries.push_back({"Done", "finished", "", "", ""});
  }
  return entries;
}

} // namespace
namespace firmius::daemon {

DaemonService::DaemonService() = default;
DaemonService::~DaemonService() { shutdown(); }

void DaemonService::start() {
  {
    std::lock_guard<std::mutex> stateLock(stateMutex_);
    if (running_) {
      return;
    }
  }
  FIRMIUS_DLOG_PHASE_SCOPE("DaemonService::start");
  std::lock_guard<std::mutex> runtimeLock(runtimeMutex_);

  broadcastInitProgress("Loading configuration...");
  {
    FIRMIUS_DLOG_PHASE_SCOPE("init.config_load");
    firmius::shared::ConfigLoader::instance().load();
  }

  broadcastInitProgress("Reading provider definitions...");
  {
    FIRMIUS_DLOG_PHASE_SCOPE("init.provider_definitions");
    firmius::provider::ProviderRegistry::instance().reloadConfigProviders(
        firmius::shared::ConfigLoader::instance().getConfig().providers);
  }

  broadcastInitProgress("Hydrating provider instances...");
  {
    FIRMIUS_DLOG_PHASE_SCOPE("init.provider_hydrate");
    firmius::provider::ProviderRegistry::instance().hydrateProviders();
  }

  broadcastInitProgress("Initializing harness...");
  auto &harness = firmius::core::Harness::instance();
  {
    FIRMIUS_DLOG_PHASE_SCOPE("init.harness");
    harness.init();
  }

  broadcastInitProgress("Loading workflow definitions...");
  {
    FIRMIUS_DLOG_PHASE_SCOPE("init.workflows");
    firmius::core::WorkflowLoader::instance().init();
  }

  broadcastInitProgress("Enumerating available models...");
  int subscriptionId = -1;
  {
    FIRMIUS_DLOG_PHASE_SCOPE("init.list_models");
    firmius::core::Harness::instance().listAllModels();
  }

  broadcastInitProgress("Subscribing to core events...");
  {
    FIRMIUS_DLOG_PHASE_SCOPE("init.subscribe_core_events");
    subscriptionId = harness.subscribe(
        [this](const firmius::shared::AppEvent &event) { emitCoreEvent(event); });
  }

  broadcastInitProgress("Restoring persisted threads...");
  // Harness::init() already loads threads, but we broadcast after so
  // clients know the restore step happened.

  broadcastInitProgress("Daemon ready.");
  std::lock_guard<std::mutex> stateLock(stateMutex_);
  harnessSubscriptionId_ = subscriptionId;
  running_ = true;
  {
    std::lock_guard<std::mutex> readyLock(readyMutex_);
    ready_ = true;
  }
  readyCv_.notify_all();
  FIRMIUS_DLOG_PHASE("daemon ready");
}

bool DaemonService::waitForReady(std::chrono::milliseconds timeout) {
  std::unique_lock<std::mutex> lock(readyMutex_);
  return readyCv_.wait_for(lock, timeout, [this] { return ready_; });
}

void DaemonService::shutdown() {
  int harnessSubscriptionId = -1;
  {
    std::lock_guard<std::mutex> stateLock(stateMutex_);
    if (!running_) {
      return;
    }
    running_ = false;
    harnessSubscriptionId = std::exchange(harnessSubscriptionId_, -1);
  }

  std::lock_guard<std::mutex> runtimeLock(runtimeMutex_);
  auto &harness = firmius::core::Harness::instance();
  if (harnessSubscriptionId >= 0) {
    harness.unsubscribe(harnessSubscriptionId);
  }
  harness.shutdown();

  // Cancel + join any in-flight /connect wizard finalize workers before we
  // wipe sessions, so they don't fire into freed listeners on the way out.
  {
    std::vector<ConnectSession> toJoin;
    {
      std::lock_guard<std::mutex> lk(connectSessionsMutex_);
      for (auto &[_, sess] : connectSessionsByClient_) {
        if (sess.cancelled) sess.cancelled->store(true);
        toJoin.push_back(std::move(sess));
      }
      connectSessionsByClient_.clear();
    }
    for (auto &sess : toJoin) {
      if (sess.finalizeWorker.joinable()) {
        sess.finalizeWorker.join();
      }
    }
  }

  std::lock_guard<std::mutex> stateLock(stateMutex_);
  subscriptions_.clear();
  sessions_.clear();
}

bool DaemonService::running() const {
  std::lock_guard<std::mutex> lock(stateMutex_);
  return running_;
}

DaemonPingResponse DaemonService::ping() const {
  DaemonPingResponse response;
#if defined(_WIN32)
  response.pid = static_cast<int>(GetCurrentProcessId());
#else
  response.pid = static_cast<int>(::getpid());
#endif
  return response;
}
DaemonAuditEmitRuntimeEventResponse
DaemonService::auditEmitRuntimeEvent(const DaemonAuditEmitRuntimeEventRequest &request) {
  if (request.eventType.empty()) {
    throw std::runtime_error("daemon.auditEmitRuntimeEvent requires event_type");
  }
  if (request.threadId.empty()) {
    throw std::runtime_error("daemon.auditEmitRuntimeEvent requires thread_id");
  }
  if (request.agentId.empty()) {
    throw std::runtime_error("daemon.auditEmitRuntimeEvent requires agent_id");
  }

  rapidjson::Document doc;
  doc.SetObject();
  auto &allocator = doc.GetAllocator();

  if (request.eventType == "agent_thinking") {
    doc.AddMember("text", rapidjson::Value(request.text.c_str(), allocator),
                  allocator);
  } else if (request.eventType == "agent_text") {
    doc.AddMember("text", rapidjson::Value(request.text.c_str(), allocator),
                  allocator);
  } else if (request.eventType == "agent_tool_call") {
    doc.AddMember("toolCallId",
                  rapidjson::Value(request.toolCallId.c_str(), allocator),
                  allocator);
    doc.AddMember("toolName",
                  rapidjson::Value(request.toolName.c_str(), allocator),
                  allocator);
    doc.AddMember("toolArgs",
                  rapidjson::Value(request.toolArgsJson.c_str(), allocator),
                  allocator);
  } else if (request.eventType == "agent_finished" ||
             request.eventType == "agent_turn_completed" ||
             request.eventType == "user_message_sent" ||
             request.eventType == "message_queued" ||
             request.eventType == "message_dequeued") {
    // Minimal body for completion/status events
    doc.AddMember("type",
                  rapidjson::Value(request.eventType.c_str(), allocator),
                  allocator);
  } else {
    throw std::runtime_error("daemon.auditEmitRuntimeEvent unsupported event_type");
  }

  rapidjson::StringBuffer buffer;
  rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
  doc.Accept(writer);
  const std::string eventJson = buffer.GetString();
  emitRuntimeEventToFocusedClients(request.eventType, request.threadId, request.agentId,
                                   eventJson);

  DaemonAuditEmitRuntimeEventResponse response;
  response.emitted = true;
  response.runtimeEventType = request.eventType;
  response.threadId = request.threadId;
  response.agentId = request.agentId;
  return response;
}


ClientHelloResponse DaemonService::registerClient(const ClientHelloRequest &request) {
  if (request.protocolVersion != kProtocolVersion) {
    throw std::runtime_error("client.hello protocol_version mismatch");
  }
  if (request.identity.clientId.empty()) {
    throw std::runtime_error("client.hello requires identity.client_id");
  }
  ClientSessionSnapshot snapshot;
  snapshot.identity = request.identity;
  snapshot.presence = request.presence;
  snapshot.focusedThreadId = request.focusedThreadId;
  snapshot.focusedAgentId = request.focusedAgentId;
  snapshot.connectedAtMs = nowMs();
  snapshot.lastSeenAtMs = snapshot.connectedAtMs;
  {
    std::lock_guard<std::mutex> lock(stateMutex_);
    sessions_[request.identity.clientId] = snapshot;
  }
  emitSessionEvent(DaemonEventKind::ClientSessionRegistered, snapshot);
  return ClientHelloResponse{kProtocolVersion, snapshot};
}

bool DaemonService::unregisterClient(const std::string &clientId) {
  // If this client had a /connect wizard in flight, cancel + join its worker
  // BEFORE we tear down sessions/subscriptions. Otherwise the finalize worker
  // could try to emit a ConnectProgress event into a freed listener.
  cancelConnectSessionForClientLocked(clientId);

  std::optional<ClientSessionSnapshot> removed;
  {
    std::lock_guard<std::mutex> lock(stateMutex_);
    subscriptions_.erase(clientId);
    auto it = sessions_.find(clientId);
    if (it == sessions_.end()) {
      return false;
    }
    removed = it->second;
    sessions_.erase(it);
  }
  if (removed.has_value() && !removed->focusedThreadId.empty()) {
    bool threadStillNeeded = false;
    {
      std::lock_guard<std::mutex> lock(stateMutex_);
      for (const auto &[_, session] : sessions_) {
        if (session.focusedThreadId == removed->focusedThreadId) {
          threadStillNeeded = true;
          break;
        }
      }
    }
    if (!threadStillNeeded) {
      firmius::core::Harness::instance().releaseThreadLock(removed->focusedThreadId);
    }
  }
  emitSessionEvent(DaemonEventKind::ClientSessionDisconnected, *removed);
  return true;
}

std::vector<ClientSessionSnapshot> DaemonService::listClients() const {
  std::lock_guard<std::mutex> lock(stateMutex_);
  std::vector<ClientSessionSnapshot> snapshots;
  snapshots.reserve(sessions_.size());
  for (const auto &[_, session] : sessions_) {
    snapshots.push_back(session);
  }
  return snapshots;
}

std::vector<firmius::shared::ThreadMetadata> DaemonService::listThreads() const {
  std::lock_guard<std::mutex> runtimeLock(runtimeMutex_);
  return firmius::core::Harness::instance().listThreads();
}

std::vector<ThreadOverview>
DaemonService::listThreadOverviews(const std::string &clientId,
                                   const ThreadOverviewRequest &request) const {
  std::vector<ClientSessionSnapshot> sessions;
  {
    std::lock_guard<std::mutex> lock(stateMutex_);
    sessions.reserve(sessions_.size());
    for (const auto &[_, session] : sessions_) {
      sessions.push_back(session);
    }
  }

  const std::string requestedCwd = normalizePathForComparison(request.cwd);

  std::lock_guard<std::mutex> runtimeLock(runtimeMutex_);
  auto &harness = firmius::core::Harness::instance();
  auto threads = harness.listThreads();
  std::vector<ThreadOverview> out;
  out.reserve(threads.size());

  for (const auto &thread : threads) {
    if (!requestedCwd.empty() &&
        normalizePathForComparison(thread.cwd) != requestedCwd) {
      continue;
    }

    ThreadOverview overview;
    overview.thread = thread;
    overview.agentCount = harness.listAgents(thread.threadId).size();
    overview.artifactCount = harness.listArtifacts(thread.threadId).size();
    overview.lockOwnerPid = harness.getThreadLockOwnerPid(thread.threadId);

    if (overview.lockOwnerPid > 0) {
      for (const auto &session : sessions) {
        if (session.identity.pid != overview.lockOwnerPid) {
          continue;
        }
        overview.lockOwnerClientId = session.identity.clientId;
        overview.lockOwnerUiKind = session.identity.uiKind;
        overview.lockedByOtherClient = session.identity.clientId != clientId;
        break;
      }
    }

    out.push_back(std::move(overview));
  }

  std::sort(out.begin(), out.end(),
            [](const auto &lhs, const auto &rhs) {
              return lhs.thread.lastActiveAt > rhs.thread.lastActiveAt;
            });
  return out;
}

ThreadSnapshot DaemonService::getThread(const std::string &clientId,
                                        const ThreadsOpenRequest &request) const {
  const std::string threadId = resolveThreadIdForRequest(clientId, request.threadId);
  if (threadId.empty()) {
    throw std::runtime_error("threads.get requires a target thread");
  }
  std::string focusedThreadId;
  std::string focusedAgentId;
  {
    std::lock_guard<std::mutex> lock(stateMutex_);
    auto it = sessions_.find(clientId);
    if (it != sessions_.end()) {
      focusedThreadId = it->second.focusedThreadId;
      focusedAgentId = it->second.focusedAgentId;
    }
  }
  std::lock_guard<std::mutex> runtimeLock(runtimeMutex_);
  return buildThreadSnapshotLocked(threadId, focusedThreadId, focusedAgentId);
}

ThreadsOpenResponse DaemonService::openThread(const std::string &clientId,
                                              const ThreadsOpenRequest &request) {
  ThreadsOpenResponse response;
  std::optional<HookStateSnapshot> hookSnapshot;
  std::lock_guard<std::mutex> runtimeLock(runtimeMutex_);
  auto &harness = firmius::core::Harness::instance();
  response.opened = harness.switchThread(request.threadId);
  if (response.opened) {
    response.thread = harness.getThreadMetadata(request.threadId);
    response.focusedAgentId = harness.focusedAgentId();
    if (response.focusedAgentId.empty()) {
      const auto agentIds = harness.listAgents(request.threadId);
      if (!agentIds.empty()) {
        response.focusedAgentId = agentIds.front();
      }
    }
    hookSnapshot = buildHookStateSnapshotLocked(
        HooksStateRequest{request.threadId, "", "", 24});
    std::lock_guard<std::mutex> lock(stateMutex_);
    updateSessionFocusLocked(clientId, response.thread.threadId,
                             response.focusedAgentId);
  }
  if (hookSnapshot.has_value()) {
    emitHookStateEvent(*hookSnapshot);
  }
  return response;
}

ThreadSnapshot DaemonService::focusThread(const std::string &clientId,
                                          const ThreadsOpenRequest &request) {
  auto opened = openThread(clientId, request);
  if (!opened.opened) {
    throw std::runtime_error("failed to focus thread");
  }
  return getThread(clientId, request);
}

ThreadsCreateResponse DaemonService::createThread(const std::string &clientId,
                                                  const ThreadsCreateRequest &request) {
  std::lock_guard<std::mutex> runtimeLock(runtimeMutex_);
  auto &harness = firmius::core::Harness::instance();
  const std::string threadId =
      harness.newThread({}, request.cwd, request.leadPersona, request.initialMode);
  ThreadsCreateResponse response;
  response.thread = harness.getThreadMetadata(threadId);
  response.focusedAgentId = harness.focusedAgentId();
  if (response.focusedAgentId.empty()) {
    const auto agentIds = harness.listAgents(threadId);
    if (!agentIds.empty()) {
      response.focusedAgentId = agentIds.front();
    }
  }
  {
    std::lock_guard<std::mutex> lock(stateMutex_);
    updateSessionFocusLocked(clientId, response.thread.threadId, response.focusedAgentId);
  }
  return response;
}

ThreadsSendResponse DaemonService::sendToThread(const std::string &clientId,
                                                const ThreadsSendRequest &request) {
  std::string targetThreadId = request.threadId;
  std::string targetAgentId = request.agentId;
  {
    std::lock_guard<std::mutex> lock(stateMutex_);
    auto it = sessions_.find(clientId);
    if (it == sessions_.end()) {
      throw std::runtime_error("unknown client session");
    }
    if (targetThreadId.empty()) {
      targetThreadId = it->second.focusedThreadId;
    }
    if (targetAgentId.empty()) {
      targetAgentId = it->second.focusedAgentId;
    }
  }
  if (targetThreadId.empty()) {
    throw std::runtime_error("threads.send requires a target thread");
  }

  std::lock_guard<std::mutex> runtimeLock(runtimeMutex_);
  auto &harness = firmius::core::Harness::instance();
  if (!harness.sendToThreadAgent(targetThreadId, targetAgentId, request.text,
                                 request.images)) {
    throw std::runtime_error(
        "failed to dispatch request to target thread/agent");
  }
  ThreadsSendResponse response;
  response.accepted = true;
  response.threadId = targetThreadId;
  response.focusedAgentId = targetAgentId;
  {
    std::lock_guard<std::mutex> lock(stateMutex_);
    updateSessionFocusLocked(clientId, response.threadId, response.focusedAgentId);
  }
  return response;
}

AgentTreeSnapshot DaemonService::listAgents(const std::string &clientId,
                                            const AgentTargetRequest &request) const {
  const std::string threadId = resolveThreadIdForRequest(clientId, request.threadId);
  if (threadId.empty()) {
    throw std::runtime_error("agents.list requires a target thread");
  }
  std::string focusedAgentId;
  {
    std::lock_guard<std::mutex> lock(stateMutex_);
    auto it = sessions_.find(clientId);
    if (it != sessions_.end() && it->second.focusedThreadId == threadId) {
      focusedAgentId = it->second.focusedAgentId;
    }
  }
  std::lock_guard<std::mutex> runtimeLock(runtimeMutex_);
  AgentTreeSnapshot snapshot;
  snapshot.threadId = threadId;
  snapshot.focusedAgentId = focusedAgentId;
  snapshot.agents = buildAgentSnapshotsLocked(threadId, focusedAgentId);
  return snapshot;
}

std::optional<AgentRuntimeSnapshot>
DaemonService::getAgent(const std::string &clientId,
                        const AgentTargetRequest &request) const {
  const std::string threadId = resolveThreadIdForRequest(clientId, request.threadId);
  const std::string agentId =
      resolveAgentIdForRequest(clientId, threadId, request.agentId);
  if (threadId.empty() || agentId.empty()) {
    return std::nullopt;
  }
  std::string focusedAgentId;
  {
    std::lock_guard<std::mutex> lock(stateMutex_);
    auto it = sessions_.find(clientId);
    if (it != sessions_.end() && it->second.focusedThreadId == threadId) {
      focusedAgentId = it->second.focusedAgentId;
    }
  }
  std::lock_guard<std::mutex> runtimeLock(runtimeMutex_);
  return buildAgentSnapshotLocked(threadId, agentId, focusedAgentId);
}

std::optional<AgentTodoSnapshot>
DaemonService::getAgentTodo(const std::string &clientId,
                            const AgentTargetRequest &request) const {
  const std::string threadId = resolveThreadIdForRequest(clientId, request.threadId);
  const std::string agentId =
      resolveAgentIdForRequest(clientId, threadId, request.agentId);
  if (threadId.empty() || agentId.empty()) {
    return std::nullopt;
  }
  std::lock_guard<std::mutex> runtimeLock(runtimeMutex_);
  return buildAgentTodoSnapshotLocked(threadId, agentId);
}

std::optional<AgentRuntimeSnapshot>
DaemonService::focusAgent(const std::string &clientId,
                          const AgentTargetRequest &request) {
  const std::string threadId = resolveThreadIdForRequest(clientId, request.threadId);
  const std::string agentId =
      resolveAgentIdForRequest(clientId, threadId, request.agentId);
  if (threadId.empty() || agentId.empty()) {
    return std::nullopt;
  }
  std::lock_guard<std::mutex> runtimeLock(runtimeMutex_);
  auto &harness = firmius::core::Harness::instance();
  if (!harness.switchThread(threadId) || !harness.setFocusedAgent(agentId)) {
    return std::nullopt;
  }
  {
    std::lock_guard<std::mutex> lock(stateMutex_);
    updateSessionFocusLocked(clientId, threadId, agentId);
  }
  return buildAgentSnapshotLocked(threadId, agentId, agentId);
}

std::optional<AgentRuntimeSnapshot>
DaemonService::compactAgent(const AgentTargetRequest &request) {
  std::lock_guard<std::mutex> runtimeLock(runtimeMutex_);
  const auto liveAgent =
      firmius::core::AgentRegistry::instance().getAgent(request.agentId);
  if (!liveAgent) {
    return std::nullopt;
  }
  firmius::core::Engine::instance().compactAgent(request.agentId);
  const auto threadId =
      liveAgent->getContext().history ? liveAgent->getContext().history->threadId : "";
  return buildAgentSnapshotLocked(threadId, request.agentId, request.agentId);
}

std::optional<AgentRuntimeSnapshot>
DaemonService::interruptAgent(const std::string &clientId,
                              const AgentTargetRequest &request) {
  const std::string threadId =
      resolveThreadIdForRequest(clientId, request.threadId);
  const std::string agentId =
      resolveAgentIdForRequest(clientId, threadId, request.agentId);
  if (threadId.empty() || agentId.empty()) {
    return std::nullopt;
  }

  std::lock_guard<std::mutex> runtimeLock(runtimeMutex_);
  auto &harness = firmius::core::Harness::instance();
  harness.abortAgent(threadId, agentId);
  {
    std::lock_guard<std::mutex> lock(stateMutex_);
    updateSessionFocusLocked(clientId, threadId, agentId);
  }
  return buildAgentSnapshotLocked(threadId, agentId, agentId);
}

std::optional<AgentRuntimeSnapshot>
DaemonService::abortAndFlushQueuedMessages(const std::string &clientId,
                                           const AgentTargetRequest &request) {
  const std::string threadId =
      resolveThreadIdForRequest(clientId, request.threadId);
  const std::string agentId =
      resolveAgentIdForRequest(clientId, threadId, request.agentId);
  if (threadId.empty() || agentId.empty()) {
    return std::nullopt;
  }

  std::lock_guard<std::mutex> runtimeLock(runtimeMutex_);
  auto &harness = firmius::core::Harness::instance();
  harness.abortAgentAndFlushQueuedMessages(threadId, agentId);
  {
    std::lock_guard<std::mutex> lock(stateMutex_);
    updateSessionFocusLocked(clientId, threadId, agentId);
  }
  return buildAgentSnapshotLocked(threadId, agentId, agentId);
}

std::vector<ProcessSnapshot>
DaemonService::listProcesses(const ProcessesListRequest &request) const {
  std::lock_guard<std::mutex> runtimeLock(runtimeMutex_);
  return buildProcessSnapshotsLocked(request.threadId, request.agentId);
}

std::optional<ProcessSnapshot>
DaemonService::getProcess(const ProcessesGetRequest &request) const {
  std::lock_guard<std::mutex> runtimeLock(runtimeMutex_);
  return buildProcessSnapshotLocked(request.threadId, request.agentId,
                                    request.processId);
}

ProcessRuntimeSummary
DaemonService::focusProcessState(const std::string &clientId,
                                 const ProcessesListRequest &request) const {
  const std::string threadId = resolveThreadIdForRequest(clientId, request.threadId);
  const std::string agentId =
      resolveAgentIdForRequest(clientId, threadId, request.agentId);
  ProcessRuntimeSummary summary;
  summary.threadId = threadId;
  summary.agentId = agentId;
  auto processes = listProcesses(ProcessesListRequest{threadId, agentId});
  for (const auto &process : processes) {
    summary.activeProcessIds.push_back(process.processId);
    if (process.running) {
      ++summary.runningCount;
    }
    if (process.blocking) {
      summary.blockingProcessIds.push_back(process.processId);
      ++summary.blockingCount;
    }
  }
  return summary;
}

std::optional<TranscriptSnapshot>
DaemonService::getTranscript(const std::string &clientId,
                             const TranscriptGetRequest &request) const {
  std::string threadId = resolveThreadIdForRequest(clientId, request.threadId);
  std::string agentId = resolveAgentIdForRequest(clientId, threadId, request.agentId);
  if (threadId.empty()) {
    return std::nullopt;
  }
  if (agentId.empty()) {
    std::lock_guard<std::mutex> runtimeLock(runtimeMutex_);
    auto agentIds = firmius::core::Harness::instance().listAgents(threadId);
    if (!agentIds.empty()) {
      agentId = agentIds.front();
    }
  }
  if (agentId.empty()) {
    return std::nullopt;
  }
  std::lock_guard<std::mutex> runtimeLock(runtimeMutex_);
  return buildTranscriptSnapshotLocked(threadId, agentId);
}

std::vector<ToolCallSnapshot>
DaemonService::listToolCalls(const std::string &clientId,
                             const ToolCallsListRequest &request) const {
  std::string threadId = resolveThreadIdForRequest(clientId, request.threadId);
  std::string agentId = request.agentId;
  std::lock_guard<std::mutex> runtimeLock(runtimeMutex_);
  return buildToolCallSnapshotsLocked(threadId, agentId);
}

SubagentActivitySnapshot
DaemonService::subagentActivity(const std::string &clientId,
                                const SubagentsActivityRequest &request) const {
  std::string threadId = resolveThreadIdForRequest(clientId, request.threadId);
  std::string agentId = request.agentId;
  std::lock_guard<std::mutex> runtimeLock(runtimeMutex_);
  return buildSubagentActivitySnapshotLocked(threadId, agentId);
}

PermissionQueueSnapshot
DaemonService::getPermissionQueue(const std::string &clientId,
                                  const PermissionModeRequest &request) const {
  const std::string threadId = resolveThreadIdForRequest(clientId, "");
  std::lock_guard<std::mutex> runtimeLock(runtimeMutex_);
  auto &harness = firmius::core::Harness::instance();
  PermissionQueueSnapshot snapshot;
  snapshot.threadId = threadId;
  snapshot.activeModeId = harness.policyEngine().activeMode().id;
  for (const auto &m : harness.policyEngine().listModes()) {
    PermissionModeWire w;
    w.id = m.id;
    w.name = m.name;
    w.description = m.description;
    w.builtIn = m.builtIn;
    snapshot.modes.push_back(std::move(w));
  }
  snapshot.pending = harness.listPendingPermissionEscalations(threadId);
  (void)request;
  return snapshot;
}

PermissionQueueSnapshot
DaemonService::setPermissionMode(const std::string &clientId,
                                 const PermissionModeUpdateRequest &request) {
  std::lock_guard<std::mutex> runtimeLock(runtimeMutex_);
  auto &harness = firmius::core::Harness::instance();
  if (!harness.policyEngine().setActiveMode(request.modeId)) {
    throw std::runtime_error("permissions.setMode: unknown mode_id '" +
                              request.modeId + "'");
  }
  // Build snapshot mirroring getPermissionQueue.
  const std::string threadId = resolveThreadIdForRequest(clientId, "");
  PermissionQueueSnapshot snapshot;
  snapshot.threadId = threadId;
  snapshot.activeModeId = harness.policyEngine().activeMode().id;
  for (const auto &m : harness.policyEngine().listModes()) {
    PermissionModeWire w{m.id, m.name, m.description, m.builtIn};
    snapshot.modes.push_back(std::move(w));
  }
  snapshot.pending = harness.listPendingPermissionEscalations(threadId);
  return snapshot;
}

PermissionCreateModeResponse DaemonService::createPermissionMode(
    const PermissionCreateModeRequest &request) {
  std::lock_guard<std::mutex> runtimeLock(runtimeMutex_);
  PermissionCreateModeResponse response;
  if (request.name.empty()) {
    response.errorMessage = "name is required";
    return response;
  }
  firmius::core::PermissionMode mode;
  mode.id = request.id;
  mode.name = request.name;
  mode.description = request.description;
  auto id = firmius::core::Harness::instance().policyEngine().createMode(
      std::move(mode), request.seedFromActive);
  if (id.empty()) {
    response.errorMessage = "mode id or name already exists";
    return response;
  }
  response.modeId = id;
  return response;
}

PermissionRenameModeResponse DaemonService::renamePermissionMode(
    const PermissionRenameModeRequest &request) {
  std::lock_guard<std::mutex> runtimeLock(runtimeMutex_);
  PermissionRenameModeResponse response;
  if (request.newName.empty()) {
    response.errorMessage = "new_name is required";
    return response;
  }
  response.ok = firmius::core::Harness::instance().policyEngine().renameMode(
      request.modeId, request.newName);
  if (!response.ok) {
    response.errorMessage = "rename failed (built-in or duplicate name)";
  }
  return response;
}

PermissionDeleteModeResponse DaemonService::deletePermissionMode(
    const PermissionDeleteModeRequest &request) {
  std::lock_guard<std::mutex> runtimeLock(runtimeMutex_);
  PermissionDeleteModeResponse response;
  response.removed = firmius::core::Harness::instance().policyEngine()
                          .deleteMode(request.modeId);
  if (!response.removed) {
    response.errorMessage = "delete failed (built-in, active, or unknown)";
  }
  return response;
}

bool DaemonService::resolvePermission(const PermissionResolveRequest &request) {
  std::lock_guard<std::mutex> runtimeLock(runtimeMutex_);
  return firmius::core::Harness::instance().resolvePermissionEscalation(
      request.requestId, request.response);
}

bool DaemonService::resolvePermissionWithRules(
    const PermissionResolveWithRulesRequest &request) {
  std::lock_guard<std::mutex> runtimeLock(runtimeMutex_);
  return firmius::core::Harness::instance()
      .resolvePermissionEscalationWithRules(request.requestId,
                                             request.selectedSuggestionIds);
}

namespace {
PolicyRuleWire toWire(const firmius::core::PolicyRule &r) {
  PolicyRuleWire w;
  w.id = r.id;
  w.category = r.category;
  w.decision = firmius::core::decisionToWire(r.decision);
  w.scope = firmius::core::scopeToWire(r.scope);
  w.comment = r.comment;
  w.match = r.match;
  w.createdAt = r.createdAt;
  w.expiresAt = r.expiresAt;
  return w;
}
firmius::core::PolicyRule fromWire(const PolicyRuleWire &w) {
  firmius::core::PolicyRule r;
  r.id = w.id;
  r.category = w.category;
  r.decision = firmius::core::decisionFromWire(w.decision);
  r.scope = firmius::core::scopeFromWire(w.scope);
  r.comment = w.comment;
  r.match = w.match;
  r.createdAt = w.createdAt;
  r.expiresAt = w.expiresAt;
  return r;
}
} // namespace

PermissionListRulesResponse DaemonService::listPolicyRules(
    const PermissionListRulesRequest &) {
  std::lock_guard<std::mutex> runtimeLock(runtimeMutex_);
  auto &engine = firmius::core::Harness::instance().policyEngine();
  PermissionListRulesResponse response;
  for (const auto &r : engine.listRules()) {
    response.rules.push_back(toWire(r));
  }
  // Merge user + project category defaults; project trumps user.
  auto user = engine.userDocument();
  auto project = engine.projectDocument();
  for (const auto &[k, v] : user.categoryDefaults.byCategory) {
    response.categoryDefaults[k] = firmius::core::decisionToWire(v);
  }
  for (const auto &[k, v] : project.categoryDefaults.byCategory) {
    response.categoryDefaults[k] = firmius::core::decisionToWire(v);
  }
  response.defaultDecision =
      firmius::core::decisionToWire(user.defaultDecision);
  return response;
}

PermissionUpsertRuleResponse DaemonService::upsertPolicyRule(
    const PermissionUpsertRuleRequest &request) {
  std::lock_guard<std::mutex> runtimeLock(runtimeMutex_);
  PermissionUpsertRuleResponse response;
  try {
    auto rule = fromWire(request.rule);
    response.ruleId = firmius::core::Harness::instance()
                          .policyEngine().upsertRule(std::move(rule));
  } catch (const std::exception &e) {
    response.errorMessage = e.what();
  }
  return response;
}

PermissionDeleteRuleResponse DaemonService::deletePolicyRule(
    const PermissionDeleteRuleRequest &request) {
  std::lock_guard<std::mutex> runtimeLock(runtimeMutex_);
  PermissionDeleteRuleResponse response;
  try {
    response.removed = firmius::core::Harness::instance()
                           .policyEngine().removeRule(request.ruleId);
  } catch (const std::exception &e) {
    response.errorMessage = e.what();
  }
  return response;
}

PermissionReloadPolicyResponse DaemonService::reloadPolicy(
    const PermissionReloadPolicyRequest &) {
  std::lock_guard<std::mutex> runtimeLock(runtimeMutex_);
  PermissionReloadPolicyResponse response;
  try {
    firmius::core::Harness::instance().policyEngine().forceReload();
  } catch (const std::exception &e) {
    response.ok = false;
    response.errorMessage = e.what();
  }
  return response;
}

ModelCatalogSnapshot DaemonService::listModels(bool refresh) {
  std::lock_guard<std::mutex> runtimeLock(runtimeMutex_);
  auto &harness = firmius::core::Harness::instance();
  if (refresh) {
    harness.invalidateModelCache();
    harness.listAllModels();
  }
  ModelCatalogSnapshot snapshot;
  snapshot.models = harness.cachedModelsSnapshot();
  snapshot.fetchingProviders = harness.listProvidersFetchingModels();
  snapshot.loaded = harness.isModelsLoaded();
  snapshot.loading = !snapshot.loaded;
  return snapshot;
}

std::optional<AgentRuntimeSnapshot>
DaemonService::switchModel(const ModelSwitchRequest &request) {
  std::lock_guard<std::mutex> runtimeLock(runtimeMutex_);
  if (request.agentId.empty()) {
    if (request.variantName.empty()) {
      firmius::core::Harness::instance().switchModel(request.providerId,
                                                     request.modelId);
    } else {
      firmius::core::Harness::instance().switchModel(request.providerId,
                                                     request.modelId,
                                                     request.variantName);
    }
    AgentRuntimeSnapshot snapshot;
    snapshot.status = firmius::shared::AgentStatus::Idle;
    snapshot.providerId = request.providerId;
    snapshot.modelId = request.modelId;
    snapshot.variantName = request.variantName;
    return snapshot;
  }
  const auto liveAgent =
      firmius::core::AgentRegistry::instance().getAgent(request.agentId);
  if (!liveAgent) {
    return std::nullopt;
  }
  if (request.variantName.empty()) {
    firmius::core::Engine::instance().switchAgentModel(
        request.agentId, request.providerId, request.modelId);
  } else {
    firmius::core::Engine::instance().switchAgentModel(
        request.agentId, request.providerId, request.modelId,
        request.variantName);
  }
  const auto threadId =
      liveAgent->getContext().history ? liveAgent->getContext().history->threadId : "";
  return buildAgentSnapshotLocked(threadId, request.agentId, request.agentId);
}

ProviderCatalogSnapshot DaemonService::listProviders() const {
  ProviderCatalogSnapshot snapshot;
  const auto cfg = firmius::core::Harness::instance().getConfig();
  std::map<std::string, ProviderProfileSnapshot> merged;
  for (const auto &id :
       firmius::provider::ProviderRegistry::instance().listProviderIds()) {
    ProviderProfileSnapshot row;
    row.id = id;
    row.custom = false;
    if (auto provider = firmius::provider::ProviderRegistry::instance().getProvider(id)) {
      row.configured = provider->isConfigured();
      row.kind = provider->getProviderType() ==
                         firmius::provider::ProviderType::OAuth
                     ? "oauth"
                     : "apikey";
      row.enabled = row.configured;
    }
    merged[id] = row;
  }
  for (const auto &[id, profile] : cfg.providers) {
    ProviderProfileSnapshot row;
    row.id = id;
    row.kind = profile.kind;
    row.authMode = profile.authMode;
    row.displayName = profile.displayName;
    row.enabled = profile.enabled;
    row.custom = true;
    row.profile = profile;
    if (auto provider = firmius::provider::ProviderRegistry::instance().getProvider(id)) {
      row.configured = provider->isConfigured();
    }
    merged[id] = row;
  }
  for (auto &[_, provider] : merged) {
    snapshot.providers.push_back(std::move(provider));
  }
  return snapshot;
}

ProviderCatalogSnapshot DaemonService::updateProviderProfiles(
    const ProviderProfilesUpdateRequest &request) {
  std::lock_guard<std::mutex> runtimeLock(runtimeMutex_);
  auto &harness = firmius::core::Harness::instance();
  auto cfg = harness.getConfig();
  cfg.providers = request.providers;
  harness.updateConfig(cfg);
  harness.saveConfig();
  firmius::provider::ProviderRegistry::instance().reloadConfigProviders(cfg.providers);
  harness.invalidateModelCache();
  return listProviders();
}

ModelCatalogSnapshot DaemonService::invalidateModelCache() {
  std::lock_guard<std::mutex> runtimeLock(runtimeMutex_);
  auto &harness = firmius::core::Harness::instance();
  harness.invalidateModelCache();
  return listModels(false);
}

HistorySnapshot DaemonService::getHistory(const std::string &clientId,
                                          const HistoryGetRequest &request) const {
  std::string threadId = resolveThreadIdForRequest(clientId, request.threadId);
  std::string agentId = request.agentId;
  std::lock_guard<std::mutex> runtimeLock(runtimeMutex_);
  if (threadId.empty()) {
    return {};
  }
  if (agentId.empty()) {
    auto agentIds = firmius::core::Harness::instance().listAgents(threadId);
    if (!agentIds.empty()) {
      agentId = agentIds.front();
    }
  }
  return buildHistorySnapshotLocked(threadId, agentId, request.limit);
}

HistoryMutationResult
DaemonService::undoHistory(const std::string &clientId,
                           const HistoryUndoRequest &request) const {
  HistoryMutationResult result;
  std::string threadId = resolveThreadIdForRequest(clientId, request.threadId);
  std::string agentId = request.agentId;
  std::lock_guard<std::mutex> runtimeLock(runtimeMutex_);
  if (threadId.empty()) {
    result.message = "Missing target thread";
    return result;
  }
  if (agentId.empty()) {
    auto agentIds = firmius::core::Harness::instance().listAgents(threadId);
    if (!agentIds.empty()) {
      agentId = agentIds.front();
    }
  }
  result.threadId = threadId;
  result.agentId = agentId;
  if (agentId.empty()) {
    result.message = "Missing target agent";
    result.history = buildHistorySnapshotLocked(threadId, agentId, 20);
    return result;
  }
  try {
    auto action = firmius::core::Engine::instance().undoAgentTurnsWithRedo(
        agentId, std::max(1, request.count));
    result.applied = true;
    result.undoAction = action;
    result.redoEligibility = firmius::core::Engine::instance().evaluateTranscriptRedo(
        threadId, action.undoActionId);
    result.message = "Transcript undo applied";
  } catch (const std::exception &e) {
    result.message = e.what();
  }
  result.history = buildHistorySnapshotLocked(threadId, agentId, 20);
  return result;
}

HistoryMutationResult
DaemonService::redoHistory(const std::string &clientId,
                           const HistoryRedoRequest &request) const {
  HistoryMutationResult result;
  std::string threadId = resolveThreadIdForRequest(clientId, request.threadId);
  std::string agentId = request.agentId;
  std::lock_guard<std::mutex> runtimeLock(runtimeMutex_);
  if (threadId.empty()) {
    result.message = "Missing target thread";
    return result;
  }
  result.threadId = threadId;
  if (request.undoActionId.empty()) {
    result.message = "Missing undo action id";
    result.history = buildHistorySnapshotLocked(threadId, agentId, 20);
    return result;
  }
  firmius::core::ThreadManager tm(firmius::core::ThreadManager::defaultBasePath());
  auto action = tm.findTranscriptUndoAction(threadId, request.undoActionId);
  if (!action.has_value()) {
    result.message = "Transcript undo action not found";
    result.history = buildHistorySnapshotLocked(threadId, agentId, 20);
    return result;
  }
  if (agentId.empty()) {
    agentId = action->agentId;
  }
  if (agentId.empty()) {
    auto agentIds = firmius::core::Harness::instance().listAgents(threadId);
    if (!agentIds.empty()) {
      agentId = agentIds.front();
    }
  }
  result.agentId = agentId;
  try {
    auto eligibility =
        firmius::core::Engine::instance().evaluateTranscriptRedo(
            threadId, request.undoActionId);
    result.redoEligibility = eligibility;
    if (!eligibility.redoable) {
      result.message = eligibility.reason;
      result.history = buildHistorySnapshotLocked(threadId, agentId, 20);
      return result;
    }
    auto redo = firmius::core::Engine::instance().redoTranscriptUndoAction(
        agentId, request.undoActionId);
    if (redo.has_value()) {
      result.applied = true;
      result.redoAction = redo;
      result.message = "Transcript redo applied";
    } else {
      result.message = "Transcript redo was not applied";
    }
  } catch (const std::exception &e) {
    result.message = e.what();
  }
  result.history = buildHistorySnapshotLocked(threadId, agentId, 20);
  return result;
}

EditHistorySnapshot
DaemonService::listEdits(const std::string &clientId,
                         const EditsListRequest &request) const {
  std::string threadId = resolveThreadIdForRequest(clientId, request.threadId);
  std::string agentId = request.agentId;
  std::lock_guard<std::mutex> runtimeLock(runtimeMutex_);
  if (threadId.empty()) {
    return {};
  }
  return buildEditHistorySnapshotLocked(threadId, agentId, request.includeUndone);
}

EditMutationResult
DaemonService::undoEdit(const std::string &clientId,
                        const EditsUndoRequest &request) const {
  EditMutationResult result;
  std::string threadId = resolveThreadIdForRequest(clientId, request.threadId);
  std::string agentId = request.agentId;
  std::lock_guard<std::mutex> runtimeLock(runtimeMutex_);
  result.threadId = threadId;
  if (threadId.empty()) {
    result.message = "Missing target thread";
    return result;
  }
  if (agentId.empty()) {
    auto agentIds = firmius::core::Harness::instance().listAgents(threadId);
    if (!agentIds.empty()) {
      agentId = agentIds.front();
    }
  }
  result.agentId = agentId;
  if (request.editBatchId.empty()) {
    result.message = "Missing edit batch id";
    result.edits = buildEditHistorySnapshotLocked(threadId, agentId, true);
    return result;
  }
  result.undoEligibility =
      firmius::core::Engine::instance().evaluateEditBatchUndo(
          threadId, request.editBatchId);
  if (agentId.empty()) {
    result.message = "Missing target agent";
    result.edits = buildEditHistorySnapshotLocked(threadId, agentId, true);
    return result;
  }
  try {
    auto action = firmius::core::Engine::instance().undoEditBatch(
        agentId, request.editBatchId);
    result.applied = true;
    result.undoAction = action;
    result.message = "Edit undo applied";
  } catch (const std::exception &e) {
    result.message = e.what();
  }
  result.edits = buildEditHistorySnapshotLocked(threadId, agentId, true);
  return result;
}

EditMutationResult
DaemonService::redoEdit(const std::string &clientId,
                        const EditsRedoRequest &request) const {
  EditMutationResult result;
  std::string threadId = resolveThreadIdForRequest(clientId, request.threadId);
  std::string agentId = request.agentId;
  std::lock_guard<std::mutex> runtimeLock(runtimeMutex_);
  result.threadId = threadId;
  if (threadId.empty()) {
    result.message = "Missing target thread";
    return result;
  }
  if (request.undoActionId.empty()) {
    result.message = "Missing undo action id";
    result.edits = buildEditHistorySnapshotLocked(threadId, agentId, true);
    return result;
  }
  result.redoEligibility =
      firmius::core::Engine::instance().evaluateEditBatchRedo(
          threadId, request.undoActionId);
  if (agentId.empty()) {
    auto agentIds = firmius::core::Harness::instance().listAgents(threadId);
    if (!agentIds.empty()) {
      agentId = agentIds.front();
    }
  }
  result.agentId = agentId;
  if (agentId.empty()) {
    result.message = "Missing target agent";
    result.edits = buildEditHistorySnapshotLocked(threadId, agentId, true);
    return result;
  }
  try {
    auto action = firmius::core::Engine::instance().redoEditUndoAction(
        agentId, request.undoActionId);
    if (action.has_value()) {
      result.applied = true;
      result.redoAction = action;
      result.message = "Edit redo applied";
    } else {
      result.message = "Edit redo was not applied";
    }
  } catch (const std::exception &e) {
    result.message = e.what();
  }
  result.edits = buildEditHistorySnapshotLocked(threadId, agentId, true);
  return result;
}

std::vector<AccountSnapshot>
DaemonService::listAccounts(const AccountsRequest &request) const {
  std::vector<AccountSnapshot> accounts;
  for (const auto &account :
       firmius::core::Harness::instance().getAccounts(request.providerId)) {
    AccountSnapshot snapshot;
    snapshot.providerId = request.providerId;
    snapshot.identifier = account.identifier;
    snapshot.rateLimited = account.rateLimited;
    snapshot.backoffUntil = account.backoffUntil;
    snapshot.metadata = account.metadata;
    accounts.push_back(std::move(snapshot));
  }
  return accounts;
}

bool DaemonService::deleteAccount(const AccountDeleteRequest &request) {
  std::lock_guard<std::mutex> runtimeLock(runtimeMutex_);
  firmius::core::Harness::instance().deleteAccount(request.providerId,
                                                   request.identifier);
  return true;
}

QuotaSnapshot DaemonService::getQuotas(const QuotasRequest &request,
                                       bool refresh) const {
  QuotaSnapshot snapshot;
  snapshot.providerId = request.providerId;
  snapshot.buckets =
      refresh ? firmius::core::Harness::instance().getAllQuotas(request.providerId)
              : firmius::core::Harness::instance().getCachedAllQuotas(
                    request.providerId);
  return snapshot;
}

UserConfigSnapshot DaemonService::getConfig() const {
  firmius::shared::ConfigLoader::instance().load();
  const auto &cfg = firmius::core::Harness::instance().getConfig();
  return UserConfigSnapshot{cfg};
}

UserConfigSnapshot DaemonService::updateConfig(const ConfigUpdateRequest &request) {
  std::lock_guard<std::mutex> runtimeLock(runtimeMutex_);
  auto &harness = firmius::core::Harness::instance();
  harness.updateConfig(request.config);
  harness.saveConfig();
  return UserConfigSnapshot{harness.getConfig()};
}

RouterConfigSnapshot DaemonService::getRouterConfig() const {
  const auto &cfg = firmius::core::Harness::instance().getConfig();
  return RouterConfigSnapshot{cfg.modelRouterCategories, cfg.defaultRouteCategory,
                              cfg.enableSubagentRouteFallback,
                              cfg.subagentRouteFallbackOrder};
}

RouterConfigSnapshot
DaemonService::updateRouterConfig(const RouterConfigUpdateRequest &request) {
  std::lock_guard<std::mutex> runtimeLock(runtimeMutex_);
  auto &harness = firmius::core::Harness::instance();
  auto cfg = harness.getConfig();
  cfg.modelRouterCategories = request.categories;
  cfg.defaultRouteCategory = request.defaultRouteCategory;
  cfg.enableSubagentRouteFallback = request.enableSubagentRouteFallback;
  cfg.subagentRouteFallbackOrder = request.subagentRouteFallbackOrder;
  harness.updateConfig(cfg);
  harness.saveConfig();
  return getRouterConfig();
}

PurposesConfigSnapshot DaemonService::getPurposesConfig() const {
  return PurposesConfigSnapshot{
      firmius::core::Harness::instance().getConfig().purposeRoutes};
}

PurposesConfigSnapshot
DaemonService::updatePurposesConfig(const PurposesConfigUpdateRequest &request) {
  std::lock_guard<std::mutex> runtimeLock(runtimeMutex_);
  auto &harness = firmius::core::Harness::instance();
  auto cfg = harness.getConfig();
  cfg.purposeRoutes = request.purposeRoutes;
  harness.updateConfig(cfg);
  harness.saveConfig();
  return getPurposesConfig();
}

McpConfigSnapshot DaemonService::getMcpConfig() const {
  return McpConfigSnapshot{firmius::core::Harness::instance().getConfig().mcpServers};
}

McpConfigSnapshot
DaemonService::updateMcpConfig(const McpConfigUpdateRequest &request) {
  std::lock_guard<std::mutex> runtimeLock(runtimeMutex_);
  auto &harness = firmius::core::Harness::instance();
  auto cfg = harness.getConfig();
  cfg.mcpServers = request.servers;
  harness.updateConfig(cfg);
  harness.saveConfig();
  return getMcpConfig();
}

HookStatusSnapshot DaemonService::listHooks() const {
  HookStatusSnapshot snapshot;
  auto workflows = firmius::core::WorkflowLoader::instance().getAllWorkflows();
  for (const auto &workflow : workflows) {
    if (workflow.trigger.kind ==
        firmius::core::WorkflowTrigger::Kind::OnEvent) {
      snapshot.hookIds.push_back(workflow.id);
    }
  }
  snapshot.hookDirs = firmius::core::WorkflowLoader::instance().getHookDirs();
  snapshot.hookCount = snapshot.hookIds.size();
  return snapshot;
}

HookStatusSnapshot DaemonService::reloadHooks() {
  std::lock_guard<std::mutex> runtimeLock(runtimeMutex_);
  firmius::core::WorkflowLoader::instance().init();
  firmius::core::hooks::HookRegistry::instance().reload();
  return listHooks();
}

HookStateSnapshot DaemonService::hookState(const HooksStateRequest &request) const {
  std::lock_guard<std::mutex> runtimeLock(runtimeMutex_);
  return buildHookStateSnapshotLocked(request);
}

std::vector<WorkflowExecutionSnapshot> DaemonService::listWorkflows() const {
  std::vector<WorkflowExecutionSnapshot> workflows;
  for (const auto &workflow :
       firmius::core::WorkflowLoader::instance().getAllWorkflows()) {
    workflows.push_back(WorkflowExecutionSnapshot{
        workflow.id,
        workflow.name,
        workflow.description,
        workflow.slashCommand.value_or(""),
        workflow.trigger.kind == firmius::core::WorkflowTrigger::Kind::OnEvent});
  }
  return workflows;
}

bool DaemonService::executeWorkflow(const WorkflowExecuteRequest &request) {
  std::lock_guard<std::mutex> runtimeLock(runtimeMutex_);
  return firmius::core::Harness::instance().executeWorkflow(request.workflowId,
                                                            request.args);
}

ArtifactCatalogSnapshot
DaemonService::listArtifacts(const std::string &clientId,
                             const ThreadsOpenRequest &request) const {
  const std::string threadId = resolveThreadIdForRequest(clientId, request.threadId);
  ArtifactCatalogSnapshot snapshot;
  snapshot.threadId = threadId;
  if (!threadId.empty()) {
    snapshot.artifacts = firmius::core::Harness::instance().listArtifacts(threadId);
  }
  return snapshot;
}

EventSubscriptionResponse DaemonService::subscribe(
    const std::string &clientId, const EventSubscriptionRequest &request,
    DaemonEventListener listener) {
  std::optional<ClientSessionSnapshot> snapshot;
  std::optional<EventSubscription> subscriptionCopy;
  {
    std::lock_guard<std::mutex> lock(stateMutex_);
    EventSubscription subscription;
    subscription.listener = std::move(listener);
    subscription.eventKinds.insert(request.eventKinds.begin(), request.eventKinds.end());
    subscription.sinceSequence = request.sinceSequence;
    subscriptions_[clientId] = std::move(subscription);
    subscriptionCopy = subscriptions_[clientId];
    auto it = sessions_.find(clientId);
    if (it != sessions_.end()) {
      it->second.subscribed = true;
      it->second.lastSeenAtMs = nowMs();
      snapshot = it->second;
    }
  }
  // Replay buffered events for new subscribers.
  //
  // Two replay paths:
  //   * Reconnect (sinceSequence > 0): give the client every event newer
  //     than the last one it acked.
  //   * Fresh subscribe (sinceSequence == 0): only replay InitProgress
  //     messages, so a TUI that connects after the daemon has begun init
  //     can still see the full phase log instead of silently missing the
  //     early "Loading configuration..." / "Initializing harness..." steps.
  if (subscriptionCopy.has_value() && subscriptionCopy->listener) {
    std::vector<DaemonEventEnvelope> replay;
    {
      std::lock_guard<std::mutex> lock(stateMutex_);
      for (const auto &event : eventReplayBuffer_) {
        if (request.sinceSequence > 0) {
          if (event.sequence <= request.sinceSequence) {
            continue;
          }
        } else if (event.kind != DaemonEventKind::InitProgress) {
          continue;
        }
        const std::string eventName = event.runtimeEventType.empty()
                                          ? "daemon_event"
                                          : event.runtimeEventType;
        if (!subscriptionWantsEvent(subscriptionCopy->eventKinds, eventName)) {
          continue;
        }
        replay.push_back(event);
      }
    }
    for (auto event : replay) {
      event.subscriptionTarget = clientId;
      subscriptionCopy->listener(event);
    }
  }
  if (snapshot.has_value()) {
    emitSessionEvent(DaemonEventKind::ClientSessionUpdated, *snapshot);
    if (!snapshot->focusedThreadId.empty() && subscriptionCopy.has_value() &&
        subscriptionCopy->listener) {
      std::lock_guard<std::mutex> runtimeLock(runtimeMutex_);
      if (subscriptionWantsEvent(subscriptionCopy->eventKinds, "hook_state_changed")) {
        DaemonEventEnvelope envelope;
        envelope.kind = DaemonEventKind::HookStateChanged;
        envelope.subscriptionTarget = clientId;
        envelope.serverTimestampMs = nowMs();
        envelope.hookState = buildHookStateSnapshotLocked(HooksStateRequest{snapshot->focusedThreadId, snapshot->focusedAgentId, "", 24});
        subscriptionCopy->listener(envelope);
      }
    }
  }
  return EventSubscriptionResponse{true};
}

EventSubscriptionResponse DaemonService::unsubscribe(const std::string &clientId) {
  std::optional<ClientSessionSnapshot> snapshot;
  {
    std::lock_guard<std::mutex> lock(stateMutex_);
    subscriptions_.erase(clientId);
    auto it = sessions_.find(clientId);
    if (it != sessions_.end()) {
      it->second.subscribed = false;
      it->second.lastSeenAtMs = nowMs();
      snapshot = it->second;
    }
  }
  if (snapshot.has_value()) {
    emitSessionEvent(DaemonEventKind::ClientSessionUpdated, *snapshot);
  }
  return EventSubscriptionResponse{false};
}

std::optional<ClientSessionSnapshot> DaemonService::session(
    const std::string &clientId) const {
  std::lock_guard<std::mutex> lock(stateMutex_);
  auto it = sessions_.find(clientId);
  if (it == sessions_.end()) {
    return std::nullopt;
  }
  return it->second;
}

UiSnapshot DaemonService::uiSnapshot(const std::string &clientId,
                                     const UiSnapshotRequest &request) {
  FIRMIUS_DLOG_SCOPE("rpc.ui.snapshot.get");
  // Wait for daemon to finish initialization before serving snapshot.
  {
    FIRMIUS_DLOG_SCOPE("ui.snapshot.waitForReady");
    waitForReady(std::chrono::milliseconds(30000));
  }

  UiSnapshot snapshot;
  auto sessionSnapshot = session(clientId);
  if (sessionSnapshot.has_value()) {
    snapshot.session = *sessionSnapshot;
  }

  auto &harness = firmius::core::Harness::instance();
  const std::string threadId = !request.threadId.empty()
                                   ? request.threadId
                                   : snapshot.session.focusedThreadId;
  const std::string agentId = !request.agentId.empty()
                                  ? request.agentId
                                  : snapshot.session.focusedAgentId;

  {
    FIRMIUS_DLOG_SCOPE("ui.snapshot.list_threads");
    snapshot.threads = harness.listThreads();
  }
  if (!threadId.empty()) {
    {
      FIRMIUS_DLOG_SCOPE("ui.snapshot.focused_thread");
      snapshot.focusedThread = getThread(clientId, ThreadsOpenRequest{threadId});
    }
    {
      FIRMIUS_DLOG_SCOPE("ui.snapshot.list_agents");
      snapshot.agents = listAgents(clientId, AgentTargetRequest{threadId, agentId});
    }
    if (!agentId.empty()) {
      FIRMIUS_DLOG_SCOPE("ui.snapshot.focused_agent");
      snapshot.focusedAgent = getAgent(clientId, AgentTargetRequest{threadId, agentId});
      snapshot.focusedAgentTodo = getAgentTodo(clientId, AgentTargetRequest{threadId, agentId});
    }
  }
  if (request.includeTranscript && !threadId.empty() && !agentId.empty()) {
    FIRMIUS_DLOG_SCOPE("ui.snapshot.transcript");
    snapshot.transcript = getTranscript(clientId, TranscriptGetRequest{threadId, agentId});
  }
  if (request.includeToolCalls && !threadId.empty() && !agentId.empty()) {
    FIRMIUS_DLOG_SCOPE("ui.snapshot.tool_calls+subagents");
    snapshot.toolCalls = listToolCalls(clientId, ToolCallsListRequest{threadId, agentId});
    snapshot.subagents = subagentActivity(clientId, SubagentsActivityRequest{threadId, agentId});
  }
  if (request.includeProcesses && !threadId.empty() && !agentId.empty()) {
    FIRMIUS_DLOG_SCOPE("ui.snapshot.processes");
    snapshot.processSummary = focusProcessState(clientId, ProcessesListRequest{threadId, agentId});
    snapshot.processes = listProcesses(ProcessesListRequest{threadId, agentId});
  }
  if (!threadId.empty()) {
    FIRMIUS_DLOG_SCOPE("ui.snapshot.permissions");
    snapshot.permissions = getPermissionQueue(clientId, PermissionModeRequest{});
  }
  if (request.includeCatalogs) {
    {
      FIRMIUS_DLOG_SCOPE("ui.snapshot.list_models");
      snapshot.models = listModels(false);
    }
    {
      FIRMIUS_DLOG_SCOPE("ui.snapshot.list_providers");
      snapshot.providers = listProviders();
    }
  }
  if (request.includeConfig) {
    FIRMIUS_DLOG_SCOPE("ui.snapshot.config_bundle");
    snapshot.config = getConfig();
    snapshot.router = getRouterConfig();
    snapshot.purposes = getPurposesConfig();
    snapshot.mcp = getMcpConfig();
  }
  {
    FIRMIUS_DLOG_SCOPE("ui.snapshot.hooks");
    snapshot.hooks = hookState(HooksStateRequest{threadId, agentId, "", 24});
  }
  if (!threadId.empty()) {
    FIRMIUS_DLOG_SCOPE("ui.snapshot.artifacts");
    snapshot.artifacts = listArtifacts(clientId, ThreadsOpenRequest{threadId});
  }
  if (!threadId.empty() && !agentId.empty()) {
    FIRMIUS_DLOG_SCOPE("ui.snapshot.history+edits");
    snapshot.history = getHistory(clientId, HistoryGetRequest{threadId, agentId, 20});
    snapshot.edits = listEdits(clientId, EditsListRequest{threadId, agentId, true});
  }
  {
    std::lock_guard<std::mutex> lock(stateMutex_);
    snapshot.latestEventSequence = nextEventSequence_ == 0 ? 0 : nextEventSequence_ - 1;
  }
  return snapshot;
}

BenchmarksStartResponse
DaemonService::startBenchmark(const std::string &clientId,
                              const BenchmarksStartRequest &request) {
  BenchmarksStartResponse response;

  (void)clientId;

  // NOTE: This is still a core-bridged implementation. The daemon's contract here
  // is typed start + observability surfaces; it does not synthesize workflow
  // semantics.

  // Validate benchmark id + task id and provide a default random task when missing.
  const auto canonicalBenchmark =
      firmius::core::canonicalBenchmarkId(request.benchmarkId);
  if (!canonicalBenchmark.has_value()) {
    throw std::runtime_error("Unknown benchmark_id: " + request.benchmarkId);
  }
  std::string taskId = request.taskId;
  {
    auto benchmark = firmius::core::makeBenchmark(*canonicalBenchmark, {});
    if (!benchmark) {
      throw std::runtime_error("Failed to construct benchmark: " +
                               *canonicalBenchmark);
    }
    const auto tasks = benchmark->listTasks();
    if (tasks.empty()) {
      throw std::runtime_error("Benchmark has no tasks: " + *canonicalBenchmark);
    }
    if (taskId.empty()) {
      std::random_device rd;
      std::mt19937 rng(rd());
      std::uniform_int_distribution<std::size_t> dist(0, tasks.size() - 1);
      taskId = tasks[dist(rng)];
    } else if (std::find(tasks.begin(), tasks.end(), taskId) == tasks.end()) {
      throw std::runtime_error("Unknown task_id for benchmark " +
                               *canonicalBenchmark + ": " + taskId);
    }
  }

  firmius::core::BenchmarkConfig config;
  config.hostOptions = request.hostOptions;
  config.cwd = request.cwd;
  config.personaName = request.personaName;

  // Use core's benchmark session for typed thread+agent bootstrap. Ensure the
  // session is marked with the requested benchmark/task IDs for downstream status.
  std::lock_guard<std::mutex> runtimeLock(runtimeMutex_);
  firmius::core::BenchmarkSession session(config);
  // Force bootstrap so thread_id/agent_id are populated in the response.
  (void)session.getAgent();
  firmius::core::Harness::instance().markThreadAsBenchmark(session.threadId(),
                                                          *canonicalBenchmark,
                                                          taskId);

  response.started = true;
  response.threadId = session.threadId();
  response.agentId = session.agentId();
  response.benchmarkId = *canonicalBenchmark;
  response.taskId = taskId;
  {
    std::lock_guard<std::mutex> lock(stateMutex_);
    updateSessionFocusLocked(clientId, response.threadId, response.agentId);
  }
  return response;
}

BenchmarkStatusSnapshot
DaemonService::getBenchmarkStatus(const std::string &clientId,
                                  const BenchmarksStatusRequest &request) const {
  (void)clientId;
  BenchmarkStatusSnapshot snapshot;
  snapshot.threadId = request.threadId;
  snapshot.agentId = request.agentId;

  std::lock_guard<std::mutex> runtimeLock(runtimeMutex_);
  auto &harness = firmius::core::Harness::instance();
  const auto meta = harness.getThreadMetadata(request.threadId);
  snapshot.isBenchmarkRun = meta.isBenchmarkRun;
  snapshot.benchmarkId = meta.benchmarkId;
  snapshot.taskId = meta.benchmarkTaskId;

  // Heuristic: agentLive is derived from whether the agent is currently present
  // in the core AgentRegistry.
  snapshot.agentLive =
      firmius::core::AgentRegistry::instance().getAgent(request.agentId) != nullptr;

  return snapshot;
}

BenchmarkLogsSnapshot
DaemonService::getBenchmarkLogs(const std::string &clientId,
                                const BenchmarksLogsRequest &request) const {
  (void)clientId;
  BenchmarkLogsSnapshot snapshot;
  snapshot.threadId = request.threadId;
  snapshot.agentId = request.agentId;

  // NOTE: Reconstructive surface: derives "logs" by slicing transcript turns.
  // This is not a dedicated benchmark log stream yet.
  auto transcript = getTranscript("", TranscriptGetRequest{request.threadId, request.agentId});
  if (!transcript.has_value()) {
    return snapshot;
  }

  const int limit = request.limit < 1 ? 1 : request.limit;
  const auto &turns = transcript->expandedTurns;
  const std::size_t start = turns.size() > static_cast<std::size_t>(limit)
                                ? (turns.size() - static_cast<std::size_t>(limit))
                                : 0;
  for (std::size_t i = start; i < turns.size(); ++i) {
    if (turns[i].messages.empty()) {
      continue;
    }
    // Reconstructive: best-effort extraction of the first text part from the
    // first message in each turn.
    const auto &msg = turns[i].messages.front();
    for (const auto &part : msg.content) {
      if (std::holds_alternative<firmius::shared::TextContent>(part)) {
        snapshot.lines.push_back(std::get<firmius::shared::TextContent>(part).text);
        break;
      }
    }
  }
  return snapshot;
}

void DaemonService::emitSessionEvent(DaemonEventKind kind,
                                     const ClientSessionSnapshot &session) {
  std::vector<std::pair<std::string, DaemonService::DaemonEventListener>> listeners;
  const std::string eventKind =
      kind == DaemonEventKind::ClientSessionRegistered
          ? "client_session_registered"
          : kind == DaemonEventKind::ClientSessionDisconnected
                ? "client_session_disconnected"
                : "client_session_updated";
  {
    std::lock_guard<std::mutex> lock(stateMutex_);
    for (const auto &[clientId, subscription] : subscriptions_) {
      if (subscription.listener &&
          subscriptionWantsEvent(subscription.eventKinds, eventKind)) {
        listeners.push_back({clientId, subscription.listener});
      }
    }
  }
  for (auto &[clientId, listener] : listeners) {
    DaemonEventEnvelope envelope;
    envelope.session = session;
    envelope = prepareEventEnvelope(std::move(envelope));
    listener(envelope);
  }
}

void DaemonService::emitHookStateEvent(const HookStateSnapshot &snapshot) {
  std::vector<std::pair<std::string, DaemonEventListener>> listeners;
  {
    std::lock_guard<std::mutex> lock(stateMutex_);
    for (const auto &[clientId, subscription] : subscriptions_) {
      if (subscription.listener &&
          subscriptionWantsEvent(subscription.eventKinds, "hook_state_changed")) {
        listeners.push_back({clientId, subscription.listener});
      }
    }
  }
  for (auto &[clientId, listener] : listeners) {
    DaemonEventEnvelope envelope;
    envelope.hookState = snapshot;
    envelope = prepareEventEnvelope(std::move(envelope));
    listener(envelope);
  }
}

void DaemonService::broadcastInitProgress(const std::string &message) {
  // Always go through prepareEventEnvelope so init events get assigned a
  // sequence number and end up in the replay buffer. Otherwise clients that
  // subscribe AFTER an init phase has already fired (which is the normal
  // case for the implicit-spawn TUI v2 path) silently miss those messages.
  DaemonEventEnvelope envelope;
  envelope.kind = DaemonEventKind::InitProgress;
  envelope.runtimeEventType = "init_progress";
  envelope.initMessage = message;
  envelope = prepareEventEnvelope(std::move(envelope));

  FIRMIUS_DLOG_INFOF("init_progress: %s", message.c_str());

  std::vector<DaemonEventListener> listeners;
  {
    std::lock_guard<std::mutex> lock(stateMutex_);
    for (const auto &[clientId, subscription] : subscriptions_) {
      listeners.push_back(subscription.listener);
    }
  }
  for (const auto &listener : listeners) {
    try {
      listener(envelope);
    } catch (...) {
      // Swallow listener errors during init.
    }
  }
}

void DaemonService::emitRuntimeEventToFocusedClients(
    const std::string &eventType, const std::string &eventThreadId,
    const std::string &eventAgentId, const std::string &eventJson,
    std::optional<firmius::shared::AgentStatus> agentStatus) {
  DaemonEventEnvelope baseEnvelope;
  baseEnvelope.kind = DaemonEventKind::RuntimeAppEvent;
  baseEnvelope.runtimeEventType = eventType;
  baseEnvelope.runtimeEventThreadId = eventThreadId;
  baseEnvelope.runtimeEventAgentId = eventAgentId;
  baseEnvelope.runtimeEventJson = eventJson;
  baseEnvelope.agentStatus = agentStatus;
  baseEnvelope = prepareEventEnvelope(std::move(baseEnvelope));

  std::vector<std::pair<std::string, DaemonEventListener>> listeners;
  {
    std::lock_guard<std::mutex> lock(stateMutex_);
    if (!running_) {
      return;
    }
    for (const auto &[clientId, subscription] : subscriptions_) {
      const auto sessionIt = sessions_.find(clientId);
      if (sessionIt == sessions_.end()) {
        continue;
      }
      const bool threadMatches = !eventThreadId.empty() &&
                                 sessionIt->second.focusedThreadId == eventThreadId;
      const bool agentMatches = !eventAgentId.empty() &&
                                sessionIt->second.focusedAgentId == eventAgentId;
      if (!threadMatches && !agentMatches) {
        continue;
      }
      if (subscription.listener &&
          subscriptionWantsEvent(subscription.eventKinds, eventType)) {
        listeners.push_back({clientId, subscription.listener});
      }
    }
  }
  for (auto &[clientId, listener] : listeners) {
    auto envelope = baseEnvelope;
    envelope.subscriptionTarget = clientId;
    listener(envelope);
  }
}

void DaemonService::emitCoreEvent(const firmius::shared::AppEvent &event) {
  const std::string eventType = appEventTypeName(event);
  const std::string eventThreadId = eventThreadIdForRouting(event);
  const std::string eventAgentId = appEventAgentId(event);
  if (eventType == "thread_changed") {
    return;
  }
  std::optional<firmius::shared::AgentStatus> agentStatus;
  if (!eventAgentId.empty()) {
    if (auto agent = firmius::core::AgentRegistry::instance().getAgent(eventAgentId)) {
      agentStatus = agent->getContext().state.currentStatus;
    }
  }
  const std::string eventJson = serializeAppEvent(event);
  emitRuntimeEventToFocusedClients(eventType, eventThreadId, eventAgentId, eventJson,
                                   agentStatus);

  if (!eventThreadId.empty()) {
    const auto hookSnapshot =
        buildHookStateSnapshotLocked(HooksStateRequest{eventThreadId, "", "", 24});
    const auto hookKey = hookStateChangeKey(hookSnapshot);
    bool emitHook = false;
    {
      std::lock_guard<std::mutex> lock(stateMutex_);
      auto &last = hookStateChangeKeys_[eventThreadId];
      if (last != hookKey) {
        last = hookKey;
        emitHook = true;
      }
    }
    if (emitHook) {
      emitHookStateEvent(hookSnapshot);
    }
  }
}

std::uint64_t DaemonService::nowMs() const {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count());
}

DaemonEventEnvelope
DaemonService::prepareEventEnvelope(DaemonEventEnvelope envelope) {
  std::lock_guard<std::mutex> lock(stateMutex_);
  envelope.serverTimestampMs = envelope.serverTimestampMs == 0 ? nowMs()
                                                               : envelope.serverTimestampMs;
  envelope.sequence = nextEventSequence_++;
  storeEventEnvelopeLocked(envelope);
  return envelope;
}

void DaemonService::storeEventEnvelopeLocked(
    const DaemonEventEnvelope &envelope) {
  eventReplayBuffer_.push_back(envelope);
  if (eventReplayBuffer_.size() > kMaxReplayEvents) {
    eventReplayBuffer_.erase(eventReplayBuffer_.begin(),
                             eventReplayBuffer_.begin() +
                                 static_cast<std::ptrdiff_t>(
                                     eventReplayBuffer_.size() -
                                     kMaxReplayEvents));
  }
}

void DaemonService::updateSessionFocusLocked(const std::string &clientId,
                                             const std::string &threadId,
                                             const std::string &agentId) {
  auto it = sessions_.find(clientId);
  if (it == sessions_.end()) {
    return;
  }
  it->second.focusedThreadId = threadId;
  it->second.focusedAgentId = agentId;
  it->second.lastSeenAtMs = nowMs();
}

std::string
DaemonService::resolveThreadIdForRequest(const std::string &clientId,
                                         const std::string &requestedThreadId) const {
  if (!requestedThreadId.empty()) {
    return requestedThreadId;
  }
  std::lock_guard<std::mutex> lock(stateMutex_);
  auto it = sessions_.find(clientId);
  if (it == sessions_.end()) {
    return "";
  }
  return it->second.focusedThreadId;
}

std::string
DaemonService::resolveAgentIdForRequest(const std::string &clientId,
                                        const std::string &requestedThreadId,
                                        const std::string &requestedAgentId) const {
  if (!requestedAgentId.empty()) {
    return requestedAgentId;
  }
  std::lock_guard<std::mutex> lock(stateMutex_);
  auto it = sessions_.find(clientId);
  if (it != sessions_.end() && it->second.focusedThreadId == requestedThreadId) {
    return it->second.focusedAgentId;
  }
  return "";
}

ThreadSnapshot
DaemonService::buildThreadSnapshotLocked(const std::string &threadId,
                                         const std::string &focusedThreadId,
                                         const std::string &focusedAgentId) const {
  auto &harness = firmius::core::Harness::instance();
  ThreadSnapshot snapshot;
  snapshot.thread = harness.getThreadMetadata(threadId);
  snapshot.focusedAgentId =
      focusedThreadId == threadId ? focusedAgentId : std::string();
  snapshot.agentIds = harness.listAgents(threadId);
  snapshot.artifactCount = harness.listArtifacts(threadId).size();
  snapshot.pendingPermissionCount =
      harness.listPendingPermissionEscalations(threadId).size();
  snapshot.focused = focusedThreadId == threadId;
  return snapshot;
}

std::vector<AgentRuntimeSnapshot>
DaemonService::buildAgentSnapshotsLocked(const std::string &threadId,
                                         const std::string &focusedAgentId) const {
  std::vector<AgentRuntimeSnapshot> snapshots;
  firmius::core::ThreadManager threadManager(
      firmius::core::ThreadManager::defaultBasePath());
  std::map<std::string, firmius::core::AgentManifestEntry> manifest;
  try {
    manifest = threadManager.readAgentManifest(threadId);
  } catch (...) {
    firmius::shared::Logger::instance().logDebug("DaemonService: failed to read agent manifest for thread snapshots");
  }
  for (const auto &[agentId, _] : manifest) {
    auto snapshot = buildAgentSnapshotLocked(threadId, agentId, focusedAgentId);
    if (snapshot.has_value()) {
      snapshots.push_back(*snapshot);
    }
  }
  return snapshots;
}

std::optional<AgentRuntimeSnapshot>
DaemonService::buildAgentSnapshotLocked(const std::string &threadId,
                                        const std::string &agentId,
                                        const std::string &focusedAgentId) const {
  if (threadId.empty() || agentId.empty()) {
    return std::nullopt;
  }
  firmius::core::ThreadManager threadManager(
      firmius::core::ThreadManager::defaultBasePath());
  firmius::core::AgentManifestEntry manifestEntry;
  bool haveManifest = false;
  try {
    auto manifest = threadManager.readAgentManifest(threadId);
    auto it = manifest.find(agentId);
    if (it != manifest.end()) {
      manifestEntry = it->second;
      haveManifest = true;
    }
  } catch (...) {
    firmius::shared::Logger::instance().logDebug("DaemonService: failed to read manifest entry for agent snapshot");
  }

  AgentRuntimeSnapshot snapshot;
  snapshot.threadId = threadId;
  snapshot.agentId = agentId;
  snapshot.focused = focusedAgentId == agentId;
  if (haveManifest) {
    snapshot.parentAgentId = manifestEntry.parentId;
    snapshot.persona = manifestEntry.persona;
    snapshot.friendlyName = manifestEntry.friendlyName;
    snapshot.title = manifestEntry.title;
  }
  if (auto liveAgent = firmius::core::AgentRegistry::instance().getAgent(agentId)) {
    const auto &ctx = liveAgent->getContext();
    snapshot.live = true;
    snapshot.parentAgentId = ctx.identity.parentId;
    snapshot.persona = ctx.config.personaName;
    snapshot.friendlyName = ctx.identity.friendlyName;
    snapshot.title = ctx.identity.name;
    snapshot.cwd = ctx.environment.cwd;
    snapshot.hostId = ctx.environment.identifier;
    snapshot.activeMode = ctx.state.activeMode;
    snapshot.status = ctx.state.currentStatus;
    snapshot.providerId = ctx.config.providerId;
    snapshot.modelId = ctx.config.modelId;
    snapshot.variantName = ctx.config.modelVariant;
    snapshot.maxTokens = ctx.config.maxTokens.value_or(0);
    snapshot.contextUsedTokens = ctx.aggregateMetrics.tokens.contextSize;
    snapshot.contextSentTokens = ctx.aggregateMetrics.context.sentTokens;
    if (auto provider =
            firmius::provider::ProviderRegistry::instance().getProvider(
                ctx.config.providerId)) {
      snapshot.contextWindowTokens =
          provider->getModelInfo(ctx.config.modelId).contextWindow;
    }
    snapshot.pendingToolCalls = ctx.state.pendingToolCalls;
    snapshot.ownedProcesses = ctx.state.ownedProcesses;
    snapshot.blockingProcessIds = ctx.state.blockingProcessIds;
    snapshot.fatalError = ctx.state.fatalError;
    snapshot.running = liveAgent->isRunning();
    snapshot.booting = liveAgent->isBooting();
  }
  return snapshot;
}

std::optional<AgentTodoSnapshot>
DaemonService::buildAgentTodoSnapshotLocked(const std::string &threadId,
                                            const std::string &agentId) const {
  if (threadId.empty() || agentId.empty()) {
    return std::nullopt;
  }
  firmius::core::ThreadManager tm(firmius::core::ThreadManager::defaultBasePath());
  const auto todo = tm.getAgentTodo(threadId, agentId);
  AgentTodoSnapshot snapshot;
  snapshot.threadId = threadId;
  snapshot.agentId = agentId;
  snapshot.nextId = todo.nextId;
  snapshot.items = todo.items;
  return snapshot;
}

std::vector<ProcessSnapshot>
DaemonService::buildProcessSnapshotsLocked(const std::string &threadId,
                                           const std::string &agentId) const {
  std::vector<ProcessSnapshot> snapshots;
  if (!agentId.empty()) {
    if (auto process = buildProcessSnapshotLocked(threadId, agentId, "")) {
      snapshots.push_back(*process);
    }
    return snapshots;
  }
  for (const auto &agent : buildAgentSnapshotsLocked(threadId, "")) {
    for (const auto &processId : agent.ownedProcesses) {
      auto process = buildProcessSnapshotLocked(threadId, agent.agentId, processId);
      if (process.has_value()) {
        snapshots.push_back(*process);
      }
    }
  }
  return snapshots;
}

std::optional<ProcessSnapshot>
DaemonService::buildProcessSnapshotLocked(const std::string &threadId,
                                          const std::string &agentId,
                                          const std::string &processId) const {
  auto liveAgent = firmius::core::AgentRegistry::instance().getAgent(agentId);
  if (!liveAgent) {
    return std::nullopt;
  }
  const auto &ctx = liveAgent->getContext();
  const auto processIds = ctx.state.ownedProcesses;
  if (processIds.empty()) {
    return std::nullopt;
  }
  auto makeSnapshot = [&](const std::string &id) -> std::optional<ProcessSnapshot> {
    try {
      const auto hostSnapshot =
          liveAgent->getEnvironment()->getProcessManager().inspectProcess(id);
      ProcessSnapshot snapshot;
      snapshot.threadId = threadId.empty() && ctx.history ? ctx.history->threadId : threadId;
      snapshot.agentId = agentId;
      snapshot.processId = id;
      snapshot.running = hostSnapshot.running;
      snapshot.exitCode = hostSnapshot.exitCode;
      snapshot.stdoutTail = hostSnapshot.stdoutData;
      snapshot.stderrTail = hostSnapshot.stderrData;
      snapshot.durationMs = hostSnapshot.elapsedMs;
      snapshot.systemId = hostSnapshot.systemId;
      snapshot.blocking =
          std::find(ctx.state.blockingProcessIds.begin(),
                    ctx.state.blockingProcessIds.end(),
                    id) != ctx.state.blockingProcessIds.end();
      return snapshot;
    } catch (...) {
      firmius::shared::Logger::instance().logDebug("DaemonService: failed to build process snapshot");
      return std::nullopt;
    }
  };
  if (!processId.empty()) {
    return makeSnapshot(processId);
  }
  return makeSnapshot(processIds.front());
}

std::optional<TranscriptSnapshot>
DaemonService::buildTranscriptSnapshotLocked(const std::string &threadId,
                                             const std::string &agentId) const {
  if (threadId.empty() || agentId.empty()) {
    return std::nullopt;
  }

  firmius::core::ThreadManager threadManager(
      firmius::core::ThreadManager::defaultBasePath());
  firmius::shared::AgentHistory history;
  try {
    history = threadManager.loadAgentHistory(threadId, agentId);
  } catch (...) {
    firmius::shared::Logger::instance().logDebug("DaemonService: failed to load agent history for transcript snapshot");
    return std::nullopt;
  }

  std::unordered_map<std::string, firmius::core::CompactionSnapshot> snapshotsById;
  try {
    for (const auto &snapshot :
         threadManager.loadCompactionSnapshots(threadId, agentId)) {
      snapshotsById[snapshot.compactionId] = snapshot;
    }
  } catch (...) {
    firmius::shared::Logger::instance().logDebug("DaemonService: failed to load compaction snapshots");
  }

  TranscriptSnapshot snapshot;
  snapshot.threadId = threadId;
  snapshot.agentId = agentId;
  snapshot.rawTurns = history.turns;
  snapshot.expandedTurns =
      expandCompactionTranscriptForDaemon(history.turns, snapshotsById);

  if (auto liveAgent = firmius::core::AgentRegistry::instance().getAgent(agentId)) {
    snapshot.agentTitle = liveAgent->getContext().identity.name;
    snapshot.agentFriendlyName = liveAgent->getContext().identity.friendlyName;
  } else {
    try {
      auto manifest = threadManager.readAgentManifest(threadId);
      auto it = manifest.find(agentId);
      if (it != manifest.end()) {
        snapshot.agentTitle = it->second.title;
        snapshot.agentFriendlyName = it->second.friendlyName;
      }
    } catch (...) {
      firmius::shared::Logger::instance().logDebug("DaemonService: failed to read manifest for agent title");
    }
  }
  return snapshot;
}

std::vector<ToolCallSnapshot>
DaemonService::buildToolCallSnapshotsLocked(const std::string &threadId,
                                            const std::string &agentId) const {
  std::vector<ToolCallSnapshot> snapshots;
  if (threadId.empty()) {
    return snapshots;
  }

  firmius::core::ThreadManager threadManager(
      firmius::core::ThreadManager::defaultBasePath());
  std::vector<std::string> agentIds;
  if (!agentId.empty()) {
    agentIds.push_back(agentId);
  } else {
    try {
      agentIds = threadManager.listAgents(threadId);
    } catch (...) {
      firmius::shared::Logger::instance().logDebug("DaemonService: failed to list agents for transcript snapshots");
      return snapshots;
    }
  }

  for (const auto &currentAgentId : agentIds) {
    firmius::shared::AgentHistory history;
    try {
      history = threadManager.loadAgentHistory(threadId, currentAgentId);
    } catch (...) {
      firmius::shared::Logger::instance().logDebug("DaemonService: failed to load history for agent in transcript loop");
      continue;
    }

    std::unordered_map<std::string, std::pair<bool, firmius::shared::ToolResultContent>>
        toolResults;
    std::unordered_map<std::string, std::uint64_t> toolResultTimes;
    for (const auto &turn : history.turns) {
      for (const auto &msg : turn.messages) {
        for (const auto &content : msg.content) {
          if (const auto *toolResult =
                  std::get_if<firmius::shared::ToolResultContent>(&content)) {
            toolResults[toolResult->toolCallId] = {toolResult->success, *toolResult};
            toolResultTimes[toolResult->toolCallId] = msg.timestamp;
          }
        }
      }
    }

    std::unordered_map<std::string, std::size_t> snapshotIndexById;
    for (const auto &turn : history.turns) {
      for (const auto &msg : turn.messages) {
        if (msg.role != firmius::shared::Role::Assistant) {
          continue;
        }
        for (const auto &content : msg.content) {
          const auto *toolCall =
              std::get_if<firmius::shared::ToolCallContent>(&content);
          if (!toolCall) {
            continue;
          }

          ToolCallSnapshot snapshot;
          snapshot.threadId = threadId;
          snapshot.agentId = currentAgentId;
          snapshot.toolCallId = toolCall->id;
          snapshot.toolName = toolCall->name;
          snapshot.toolArgsJson = toolCall->args;
          snapshot.summary = firmius::shared::SummarizeToolCall(
              toolCall->name, toolCall->args, firmius::shared::ToolPhase::Finished);
          snapshot.issuedAtMs = msg.timestamp;
          snapshot.status = "finished";

          auto resultIt = toolResults.find(toolCall->id);
          if (resultIt != toolResults.end()) {
            const auto &toolResult = resultIt->second.second;
            snapshot.success = resultIt->second.first;
            snapshot.resultJson = toolResult.result;
            snapshot.resultSummary = resultIt->second.first
                                         ? summarizeHistoricalToolEntryForDaemon(
                                               toolCall->name, toolCall->args,
                                               toolResult.result, true)
                                         : "";
            snapshot.errorSummary =
                resultIt->second.first ? "" : toolResult.result;
            snapshot.processId = !toolResult.processId.empty()
                                     ? toolResult.processId
                                     : extractProcessIdForDaemon(toolResult.result);
            snapshot.subagentId = !toolResult.subagentId.empty()
                                      ? toolResult.subagentId
                                      : parseSubagentResultForDaemon(toolResult.result)
                                            .agentId;
            snapshot.completedAtMs = toolResultTimes[toolCall->id];
            snapshot.status = resultIt->second.first ? "finished" : "error";
          }

          snapshotIndexById[snapshot.toolCallId] = snapshots.size();
          snapshots.push_back(std::move(snapshot));
        }
      }
    }

    if (auto liveAgent = firmius::core::AgentRegistry::instance().getAgent(currentAgentId)) {
      const auto &ctx = liveAgent->getContext();
      for (const auto &pendingId : ctx.state.pendingToolCalls) {
        auto it = snapshotIndexById.find(pendingId);
        if (it != snapshotIndexById.end()) {
          auto &snapshot = snapshots[it->second];
          if (!snapshot.success.has_value()) {
            snapshot.status = "called";
          }
        } else {
          ToolCallSnapshot snapshot;
          snapshot.threadId = threadId;
          snapshot.agentId = currentAgentId;
          snapshot.toolCallId = pendingId;
          snapshot.status = "called";
          snapshots.push_back(std::move(snapshot));
        }
      }
    }
  }

  std::sort(snapshots.begin(), snapshots.end(),
            [](const ToolCallSnapshot &lhs, const ToolCallSnapshot &rhs) {
              if (lhs.issuedAtMs != rhs.issuedAtMs) {
                return lhs.issuedAtMs < rhs.issuedAtMs;
              }
              return lhs.toolCallId < rhs.toolCallId;
            });
  return snapshots;
}

SubagentActivitySnapshot
DaemonService::buildSubagentActivitySnapshotLocked(const std::string &threadId,
                                                   const std::string &agentId) const {
  SubagentActivitySnapshot snapshot;
  snapshot.threadId = threadId;
  snapshot.agentId = agentId;
  if (threadId.empty()) {
    return snapshot;
  }

  firmius::core::ThreadManager threadManager(
      firmius::core::ThreadManager::defaultBasePath());
  std::vector<std::string> agentIds;
  if (!agentId.empty()) {
    agentIds.push_back(agentId);
  } else {
    try {
      agentIds = threadManager.listAgents(threadId);
    } catch (...) {
      firmius::shared::Logger::instance().logDebug("DaemonService: failed to list agents for subagent activity");
      return snapshot;
    }
  }

  std::unordered_map<std::string, SubagentActivityEntrySnapshot> activityByParentTool;
  std::unordered_map<std::string, std::string> childToParentTool;
  std::map<std::string, firmius::core::AgentManifestEntry> manifest;
  try {
    manifest = threadManager.readAgentManifest(threadId);
  } catch (...) {
    firmius::shared::Logger::instance().logDebug("DaemonService: failed to read manifest for subagent activity");
  }

  for (const auto &parentAgentId : agentIds) {
    firmius::shared::AgentHistory history;
    try {
      history = threadManager.loadAgentHistory(threadId, parentAgentId);
    } catch (...) {
      firmius::shared::Logger::instance().logDebug("DaemonService: failed to load history for subagent activity parent");
      continue;
    }

    std::unordered_map<std::string, firmius::shared::ToolResultContent> resultsById;
    for (const auto &turn : history.turns) {
      for (const auto &msg : turn.messages) {
        for (const auto &content : msg.content) {
          if (const auto *toolResult =
                  std::get_if<firmius::shared::ToolResultContent>(&content)) {
            resultsById[toolResult->toolCallId] = *toolResult;
          }
        }
      }
    }

    for (const auto &turn : history.turns) {
      for (const auto &msg : turn.messages) {
        if (msg.role != firmius::shared::Role::Assistant) {
          continue;
        }
        for (const auto &content : msg.content) {
          const auto *toolCall =
              std::get_if<firmius::shared::ToolCallContent>(&content);
          if (!toolCall || !isDelegateLikeTool(toolCall->name)) {
            continue;
          }

          const auto parsedArgs = parseSubagentArgsForDaemon(toolCall->args);
          std::string canonicalParentToolId = toolCall->id;
          if (isWaitLikeDelegateTool(toolCall->name, parsedArgs) &&
              !parsedArgs.agentId.empty()) {
            auto it = childToParentTool.find(parsedArgs.agentId);
            if (it != childToParentTool.end()) {
              canonicalParentToolId = it->second;
            }
          }

          auto &entry = activityByParentTool[canonicalParentToolId];
          entry.threadId = threadId;
          entry.parentAgentId = parentAgentId;
          entry.parentToolCallId = canonicalParentToolId;
          if (entry.task.empty()) {
            entry.task = parsedArgs.task;
          }
          if (entry.childTitle.empty()) {
            entry.childTitle =
                !parsedArgs.title.empty() ? parsedArgs.title : parsedArgs.name;
          }
          if (!parsedArgs.agentId.empty() && entry.childAgentId.empty()) {
            entry.childAgentId = parsedArgs.agentId;
            childToParentTool[parsedArgs.agentId] = canonicalParentToolId;
          }

          const auto resultIt = resultsById.find(toolCall->id);
          if (resultIt != resultsById.end()) {
            const auto parsedResult =
                parseSubagentResultForDaemon(resultIt->second.result);
            if (!parsedResult.agentId.empty()) {
              entry.childAgentId = parsedResult.agentId;
              childToParentTool[parsedResult.agentId] = canonicalParentToolId;
            }
            entry.fallbackUsed = entry.fallbackUsed || parsedResult.fallbackUsed;
            if (entry.routeCategory.empty()) {
              entry.routeCategory = parsedResult.routeCategory;
            }
            if (entry.attemptedCategories.empty()) {
              entry.attemptedCategories = parsedResult.attemptedCategories;
            }
            if (entry.finalSummary.empty()) {
              entry.finalSummary = parsedResult.result;
            }
            if (entry.errorText.empty()) {
              entry.errorText = parsedResult.error;
            }
            if (entry.artifactsCreated.empty()) {
              entry.artifactsCreated = parsedResult.artifactsCreated;
            }
            if (entry.artifactsUpdated.empty()) {
              entry.artifactsUpdated = parsedResult.artifactsUpdated;
            }
            if (!parsedResult.status.empty()) {
              entry.waitState = parsedResult.status;
            } else {
              entry.waitState = resultIt->second.success ? "completed" : "failed";
            }
          }
        }
      }
    }
  }

  for (auto &[_, entry] : activityByParentTool) {
    if (!entry.childAgentId.empty()) {
      auto manifestIt = manifest.find(entry.childAgentId);
      if (manifestIt != manifest.end()) {
        if (entry.childFriendlyName.empty()) {
          entry.childFriendlyName = manifestIt->second.friendlyName;
        }
        if (entry.childTitle.empty()) {
          entry.childTitle = manifestIt->second.title;
        }
      }

      firmius::shared::AgentHistory childHistory;
      bool hasChildHistory = false;
      try {
        childHistory = threadManager.loadAgentHistory(threadId, entry.childAgentId);
        hasChildHistory = !childHistory.turns.empty();
      } catch (...) {
        firmius::shared::Logger::instance().logDebug("DaemonService: failed to load child agent history for subagent activity");
      }
      if (hasChildHistory) {
        entry.activityLog = synthesizeHistoricalSubagentLogForDaemon(
            childHistory, entry.task, entry.waitState, entry.errorText);
      } else {
        if (!entry.task.empty()) {
          entry.activityLog.push_back(
              {"Task: " + entry.task, "finished", "", "", ""});
        }
        if (!entry.waitState.empty()) {
          const auto terminal = synthesizeHistoricalSubagentLogForDaemon(
              firmius::shared::AgentHistory{}, "", entry.waitState,
              entry.errorText);
          entry.activityLog.insert(entry.activityLog.end(), terminal.begin(),
                                   terminal.end());
        }
      }

      if (auto liveChild = firmius::core::AgentRegistry::instance().getAgent(
              entry.childAgentId)) {
        entry.running = liveChild->isRunning() || liveChild->isBooting();
        if (entry.running && (entry.waitState.empty() || entry.waitState == "completed")) {
          entry.waitState = "running";
        }
      }
    }

    if (!entry.running) {
      entry.outcome = subagentOutcomeStringFromWaitState(entry.waitState);
    } else {
      entry.outcome = "unknown";
    }
    snapshot.activities.push_back(entry);
  }

  std::sort(snapshot.activities.begin(), snapshot.activities.end(),
            [](const SubagentActivityEntrySnapshot &lhs,
               const SubagentActivityEntrySnapshot &rhs) {
              return lhs.parentToolCallId < rhs.parentToolCallId;
            });
  return snapshot;
}

HistorySnapshot
DaemonService::buildHistorySnapshotLocked(const std::string &threadId,
                                          const std::string &agentId,
                                          int limit) const {
  HistorySnapshot snapshot;
  snapshot.threadId = threadId;
  snapshot.agentId = agentId;
  if (threadId.empty()) {
    return snapshot;
  }
  firmius::core::ThreadManager tm(firmius::core::ThreadManager::defaultBasePath());
  snapshot.recentUndoActions =
      tm.listTranscriptUndoActions(threadId, std::max(1, limit));
  for (const auto &action : snapshot.recentUndoActions) {
    const auto eligibility = firmius::core::Engine::instance().evaluateTranscriptRedo(
        threadId, action.undoActionId);
    snapshot.redoEligibilities.push_back(eligibility);
    if (snapshot.latestRedoEligibleUndoActionId.empty() && eligibility.redoable) {
      snapshot.latestRedoEligibleUndoActionId = action.undoActionId;
    }
  }
  return snapshot;
}

EditHistorySnapshot
DaemonService::buildEditHistorySnapshotLocked(const std::string &threadId,
                                              const std::string &agentId,
                                              bool includeUndone) const {
  EditHistorySnapshot snapshot;
  snapshot.threadId = threadId;
  snapshot.agentId = agentId;
  if (threadId.empty()) {
    return snapshot;
  }
  firmius::shared::EditHistoryFilters filters;
  if (!agentId.empty()) {
    filters.agentId = agentId;
  }
  filters.includeUndone = includeUndone;
  snapshot.batches =
      firmius::core::Engine::instance().listAgentEditBatches(threadId, filters);
  for (const auto &batch : snapshot.batches) {
    snapshot.undoEligibilities.push_back(
        firmius::core::Engine::instance().evaluateEditBatchUndo(
            threadId, batch.editBatchId));
  }
  return snapshot;
}

HookStateSnapshot
DaemonService::buildHookStateSnapshotLocked(const HooksStateRequest &request) const {
  HookStateSnapshot snapshot;
  snapshot.threadId = request.threadId;
  snapshot.agentId = request.agentId;
  snapshot.hookId = request.hookId;
  firmius::core::hooks::HookState::instance().bindThread(request.threadId);
  snapshot.snapshotJson =
      firmius::core::hooks::HookState::instance().snapshotJson(request.hookId);

  auto recent = firmius::core::hooks::HookRegistry::instance().recentActivity(
      request.threadId, static_cast<std::size_t>(std::max(1, request.limit)));
  for (const auto &record : recent) {
    if (!request.agentId.empty() && record.agentId != request.agentId) {
      continue;
    }
    if (!request.hookId.empty() && record.hookId != request.hookId) {
      continue;
    }
    snapshot.recentActivity.push_back(HookActivitySnapshot{
        record.hookId,      record.threadId,   record.agentId,   record.eventName,
        record.decision,    record.outcomeLabel, record.blockReason,
        record.statusLine,  record.timestampMs, record.stateWriteCount});
    snapshot.currentStatusLines.push_back(record.statusLine);
    if (!record.blockReason.empty()) {
      snapshot.blockingReasons.push_back(record.blockReason);
    }
    snapshot.latestDecision = record.decision;
    snapshot.latestOutcomeLabel = record.outcomeLabel;
    snapshot.latestStatusLine = record.statusLine;
    snapshot.latestTimestampMs = record.timestampMs;
    snapshot.totalStateWriteCount += record.stateWriteCount;
  }
  return snapshot;
}

std::string DaemonService::hookStateChangeKey(const HookStateSnapshot &snapshot) const {
  rapidjson::Document doc;
  doc.SetObject();
  auto &allocator = doc.GetAllocator();
  auto value = toJsonValue(snapshot, allocator);
  doc.CopyFrom(value, allocator);
  return stringifyRapidJsonValue(doc);
}

ModeCatalogSnapshot DaemonService::listModes() const {
  ModeCatalogSnapshot snapshot;
  auto names = firmius::core::modes::ModeRegistry::instance().listNames();
  for (const auto& name : names) {
    auto mode = firmius::core::modes::ModeRegistry::instance().find(name);
    if (!mode) continue;
    ModeSnapshot ms;
    ms.modeId = mode->qualifiedName();
    ms.name = mode->name;
    ms.description = mode->shortDescription;
    snapshot.modes.push_back(ms);
  }
  return snapshot;
}

std::optional<ModeSnapshot> DaemonService::getMode(const ModesGetRequest& request) const {
  auto mode = firmius::core::modes::ModeRegistry::instance().find(request.modeId);
  if (!mode) return std::nullopt;
  ModeSnapshot ms;
  ms.modeId = mode->qualifiedName();
  ms.name = mode->name;
  ms.description = mode->shortDescription;
  return ms;
}

std::optional<AgentRuntimeSnapshot> DaemonService::setAgentMode(
    const std::string& clientId, const AgentsSetModeRequest& request) {
  std::lock_guard<std::mutex> lock(runtimeMutex_);
  auto tid = resolveThreadIdForRequest(clientId, request.threadId);
  auto aid = resolveAgentIdForRequest(clientId, tid, request.agentId);
  if (tid.empty() || aid.empty()) return std::nullopt;
  
  auto targetAgent = firmius::core::AgentRegistry::instance().getAgent(aid);
  if (!targetAgent) return std::nullopt;

  auto newMode = firmius::core::modes::ModeRegistry::instance().find(request.modeId);
  if (!newMode) return std::nullopt;

  targetAgent->getMutableContext().state.activeMode = newMode->qualifiedName();

  auto agents = buildAgentSnapshotsLocked(tid, aid);
  for (const auto& a : agents) {
    if (a.agentId == aid) return a;
  }
  return std::nullopt;
}

PersonaCatalogSnapshot DaemonService::listPersonas() const {
  PersonaCatalogSnapshot snapshot;
  auto names = firmius::core::PurposeLoader::listPurposes();
  for (const auto& name : names) {
    auto p = firmius::core::PurposeLoader::load(name);
    PersonaSnapshot ps;
    ps.id = p.name;
    ps.name = p.name;
    ps.title = p.title;
    ps.description = p.description;
    ps.allowedScopes = p.allowedScopes;
    snapshot.personas.push_back(ps);
  }
  return snapshot;
}

ToolCatalogSnapshot DaemonService::toolCatalog() const {
  ToolCatalogSnapshot snapshot;
  auto tools = firmius::core::Engine::instance().getToolRegistry().listToolMetadata();
  for (const auto& t : tools) {
    ToolSnapshot ts;
    ts.name = t.name;
    ts.description = t.description;
    ts.scopes = {t.scope};
    snapshot.tools.push_back(ts);
  }
  return snapshot;
}

BenchmarkCatalogSnapshot DaemonService::listSupportedBenchmarks() const {
  BenchmarkCatalogSnapshot snapshot;
  snapshot.availableBenchmarks = firmius::core::supportedBenchmarkIds();
  return snapshot;
}

HooksRecentActivitySnapshot DaemonService::recentHookActivity(const HooksRecentActivityRequest& request) const {
  HooksRecentActivitySnapshot snapshot;
  auto activities = firmius::core::hooks::HookRegistry::instance().recentActivity(request.threadId);
  for (const auto& a : activities) {
    HookActivitySnapshot activity;
    activity.hookId = a.hookId;
    activity.threadId = request.threadId;
    activity.eventName = a.eventName;
    activity.decision = a.decision;
    activity.outcomeLabel = a.outcomeLabel;
    activity.blockReason = a.blockReason;
    activity.timestampMs = a.timestampMs;
    snapshot.activities.push_back(activity);
  }
  return snapshot;
}

// ── /connect wizard ────────────────────────────────────────────────────────
//
// One active wizard per client, keyed by clientId. The wizard runs entirely
// inside the daemon. The TUI receives prompt snapshots, forwards user
// answers, then triggers an async finalize. Progress and the final
// success/failure outcome arrive over the event stream as
// DaemonEventKind::ConnectProgress.
//
// Locking discipline: connectSessionsMutex_ guards connectSessionsByClient_
// and nextConnectSessionId_. Wizard methods (nextPrompt/submitAnswer/etc.)
// are NOT thread-safe in general; we hold the mutex across calls but rely
// on the fact that any single client's RPCs are serialized by the JSON-RPC
// transport. The finalize worker drops the mutex while polling and only
// reacquires it briefly at the end to erase the session.

namespace {

std::string detectFirstUrlInPrompt(const std::string &text) {
  // Cheap manual scan to avoid pulling <regex> compile cost for one prompt.
  // Matches "https://..." or "http://..." up to the next whitespace.
  static constexpr std::string_view kPrefixes[] = {"https://", "http://"};
  for (auto prefix : kPrefixes) {
    auto pos = text.find(prefix);
    if (pos == std::string::npos) continue;
    auto end = pos;
    while (end < text.size() && !std::isspace(static_cast<unsigned char>(text[end]))) {
      ++end;
    }
    return text.substr(pos, end - pos);
  }
  return {};
}

WizardPromptSnapshot toPromptSnapshot(const firmius::WizardPrompt &p) {
  WizardPromptSnapshot snap;
  snap.message = p.message;
  snap.isSecret = p.isSecret;
  for (const auto &c : p.choices) {
    snap.choices.push_back(WizardChoiceSnapshot{c.label, c.value});
  }
  snap.allowFreeformInput = p.allowFreeformInput;
  snap.allowEmptyInput = p.allowEmptyInput;
  snap.placeholder = p.placeholder;
  snap.submitLabel = p.submitLabel;
  snap.detectedUrl = detectFirstUrlInPrompt(p.message);
  snap.isWaiting = false;
  return snap;
}

WizardPromptSnapshot makeWaitingPrompt() {
  WizardPromptSnapshot snap;
  snap.message = "Waiting for the provider to finish the authentication flow...";
  snap.isSecret = false;
  snap.allowFreeformInput = false;
  snap.allowEmptyInput = true;
  snap.placeholder = "";
  snap.submitLabel = "Waiting...";
  snap.isWaiting = true;
  return snap;
}

} // namespace

std::optional<WizardPromptSnapshot>
DaemonService::pollNextPromptLocked(ConnectSession &session) const {
  std::optional<firmius::WizardPrompt> next;
  bool isComplete = false;
  if (session.oauthWizard) {
    next = session.oauthWizard->nextPrompt();
    isComplete = session.oauthWizard->isComplete();
  } else if (session.apiKeyWizard) {
    next = session.apiKeyWizard->nextPrompt();
    isComplete = session.apiKeyWizard->isComplete();
  } else {
    return std::nullopt;
  }

  if (next.has_value()) {
    return toPromptSnapshot(*next);
  }
  // No next prompt and not yet complete → synthetic "Waiting..." prompt for
  // long-running flows like Antigravity (localhost callback) or Kiro
  // (device flow polling).
  if (!isComplete) {
    return makeWaitingPrompt();
  }
  // No prompt and complete → caller should call finalize.
  return std::nullopt;
}

void DaemonService::cancelConnectSessionForClientLocked(
    const std::string &clientId) {
  ConnectSession session;
  bool had = false;
  {
    std::lock_guard<std::mutex> lk(connectSessionsMutex_);
    auto it = connectSessionsByClient_.find(clientId);
    if (it == connectSessionsByClient_.end()) {
      return;
    }
    if (it->second.cancelled) it->second.cancelled->store(true);
    session = std::move(it->second);
    connectSessionsByClient_.erase(it);
    had = true;
  }
  if (!had) return;
  if (session.finalizeWorker.joinable()) {
    session.finalizeWorker.join();
  }
  // Wizard destructors stop their own polling threads / localhost servers.
}

void DaemonService::emitConnectProgressEvent(
    const std::string &clientId, const ConnectProgressSnapshot &snapshot) {
  DaemonEventListener listener;
  {
    std::lock_guard<std::mutex> lock(stateMutex_);
    auto it = subscriptions_.find(clientId);
    if (it == subscriptions_.end() || !it->second.listener) {
      return;
    }
    listener = it->second.listener;
  }
  DaemonEventEnvelope envelope;
  envelope.kind = DaemonEventKind::ConnectProgress;
  envelope.connectProgress = snapshot;
  envelope = prepareEventEnvelope(std::move(envelope));
  listener(envelope);
}

ConnectBeginResponse
DaemonService::beginConnect(const std::string &clientId,
                             const ConnectBeginRequest &request) {
  ConnectBeginResponse response;
  response.providerId = request.providerId;

  if (clientId.empty()) {
    response.errorMessage = "client must register (client.hello) before /connect";
    return response;
  }

  auto &registry = firmius::provider::ProviderRegistry::instance();
  registry.hydrateProviders();
  auto provider = registry.getProvider(request.providerId);
  if (!provider) {
    response.errorMessage = "Unknown provider: " + request.providerId;
    return response;
  }

  // If the client already had a wizard mid-flow, drop it before starting a new
  // one. This both enforces "one active wizard per client" and gives the user
  // a way to recover from a stuck flow by re-running /connect.
  cancelConnectSessionForClientLocked(clientId);

  ConnectSession session;
  session.clientId = clientId;
  session.providerId = request.providerId;
  session.cancelled = std::make_shared<std::atomic<bool>>(false);

  switch (provider->getProviderType()) {
  case firmius::provider::ProviderType::OAuth: {
    auto oauthProvider =
        std::dynamic_pointer_cast<firmius::provider::BaseOAuthProvider>(provider);
    if (!oauthProvider) {
      response.errorMessage =
          "Provider " + request.providerId + " is OAuth but cast failed";
      return response;
    }
    session.providerKind = "oauth";
    response.providerKind = "oauth";

    auto accounts = oauthProvider->getAccounts();
    if (!accounts.empty() && !request.addAdditional) {
      response.existingAccounts = true;
      return response;  // TUI confirms then re-calls with addAdditional=true.
    }

    auto wizard = oauthProvider->beginConnectionWizard();
    if (!wizard) {
      response.errorMessage = request.providerId +
          " does not require interactive authentication.";
      return response;
    }
    session.oauthWizard = std::move(wizard);
    break;
  }
  case firmius::provider::ProviderType::APIKey: {
    auto apiKeyProvider =
        std::dynamic_pointer_cast<firmius::provider::BaseAPIKeyProvider>(provider);
    if (!apiKeyProvider) {
      response.errorMessage =
          "Provider " + request.providerId + " is APIKey but cast failed";
      return response;
    }
    session.providerKind = "apikey";
    response.providerKind = "apikey";

    auto accounts = apiKeyProvider->getAccounts();
    if (!accounts.empty() && !request.addAdditional) {
      response.existingAccounts = true;
      return response;
    }

    auto wizard = apiKeyProvider->beginConnectionWizard();
    if (!wizard) {
      response.errorMessage = request.providerId +
          " does not require an API key.";
      return response;
    }
    session.apiKeyWizard = std::move(wizard);
    break;
  }
  }

  // Generate session id and pull the first prompt.
  std::string sessionId;
  {
    std::lock_guard<std::mutex> lk(connectSessionsMutex_);
    sessionId =
        "connect-" + clientId + "-" + std::to_string(nextConnectSessionId_++);
  }
  session.sessionId = sessionId;
  response.sessionId = sessionId;

  auto promptSnap = pollNextPromptLocked(session);
  if (promptSnap.has_value()) {
    response.prompt = std::move(promptSnap);
  } else {
    // Wizard had nothing to ask AND is already complete — go straight to
    // finalize (rare, but theoretically possible).
    response.readyToFinalize = true;
  }

  {
    std::lock_guard<std::mutex> lk(connectSessionsMutex_);
    connectSessionsByClient_[clientId] = std::move(session);
  }
  return response;
}

ConnectSubmitResponse
DaemonService::submitConnect(const std::string &clientId,
                              const ConnectSubmitRequest &request) {
  ConnectSubmitResponse response;
  std::lock_guard<std::mutex> lk(connectSessionsMutex_);
  auto it = connectSessionsByClient_.find(clientId);
  if (it == connectSessionsByClient_.end() ||
      it->second.sessionId != request.sessionId) {
    response.errorMessage = "No active /connect session for this client.";
    return response;
  }
  auto &session = it->second;

  // Don't accept submits while we're already in the synthetic-waiting state
  // OR while finalize has been kicked off — the client should have stopped
  // calling submit at that point.
  if (session.finalizeStarted) {
    response.errorMessage = "Wizard is finalizing; cannot submit answers.";
    return response;
  }

  try {
    if (session.oauthWizard) {
      session.oauthWizard->submitAnswer(request.answer);
    } else if (session.apiKeyWizard) {
      session.apiKeyWizard->submitAnswer(request.answer);
    }
  } catch (const std::exception &e) {
    response.errorMessage = std::string("Wizard rejected answer: ") + e.what();
    return response;
  }

  response.ok = true;
  auto promptSnap = pollNextPromptLocked(session);
  if (promptSnap.has_value()) {
    response.prompt = std::move(promptSnap);
  } else {
    response.readyToFinalize = true;
  }
  return response;
}

ConnectFinalizeResponse
DaemonService::finalizeConnect(const std::string &clientId,
                                const ConnectFinalizeRequest &request) {
  ConnectFinalizeResponse response;

  std::shared_ptr<std::atomic<bool>> cancelled;
  std::string sessionId;
  std::string providerId;
  {
    std::lock_guard<std::mutex> lk(connectSessionsMutex_);
    auto it = connectSessionsByClient_.find(clientId);
    if (it == connectSessionsByClient_.end() ||
        it->second.sessionId != request.sessionId) {
      response.errorMessage = "No active /connect session for this client.";
      return response;
    }
    auto &session = it->second;
    if (session.finalizeStarted) {
      // Already running. Treat as accepted so the TUI just keeps waiting for
      // the ConnectProgress event.
      response.accepted = true;
      return response;
    }
    session.finalizeStarted = true;
    cancelled = session.cancelled;
    sessionId = session.sessionId;
    providerId = session.providerId;

    // Spawn the worker. We pass `clientId` by value so it survives even if
    // the client disconnects mid-flow (the unregister path will set
    // cancelled=true and join us; we just no-op on emit).
    session.finalizeWorker = std::thread(
        [this, clientId, sessionId, providerId, cancelled]() {
          // Tell the TUI we're polling. Useful for OAuth flows that may take
          // a minute waiting for the user to click through the browser.
          {
            ConnectProgressSnapshot prog;
            prog.sessionId = sessionId;
            prog.providerId = providerId;
            prog.phase = ConnectProgressPhase::Polling;
            emitConnectProgressEvent(clientId, prog);
          }

          // Poll isComplete() without holding the connectSessionsMutex_ — the
          // wizard may take a long time and we don't want to block other
          // RPCs on this client.
          auto checkComplete = [&]() -> bool {
            std::lock_guard<std::mutex> lk(connectSessionsMutex_);
            auto it = connectSessionsByClient_.find(clientId);
            if (it == connectSessionsByClient_.end()) return true;
            auto &s = it->second;
            if (s.oauthWizard)  return s.oauthWizard->isComplete();
            if (s.apiKeyWizard) return s.apiKeyWizard->isComplete();
            return true;
          };
          while (!cancelled->load() && !checkComplete()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
          }

          if (cancelled->load()) {
            ConnectProgressSnapshot prog;
            prog.sessionId = sessionId;
            prog.providerId = providerId;
            prog.phase = ConnectProgressPhase::Cancelled;
            emitConnectProgressEvent(clientId, prog);
            return;
          }

          {
            ConnectProgressSnapshot prog;
            prog.sessionId = sessionId;
            prog.providerId = providerId;
            prog.phase = ConnectProgressPhase::Finalizing;
            emitConnectProgressEvent(clientId, prog);
          }

          // Take the wizard out of the session under the lock, then exchange
          // outside the lock to avoid blocking other RPCs from this client
          // (e.g. cancel) on the network call.
          std::unique_ptr<firmius::OAuthWizard> oauthWizard;
          std::unique_ptr<firmius::provider::APIKeyWizard> apiKeyWizard;
          std::string providerKind;
          {
            std::lock_guard<std::mutex> lk(connectSessionsMutex_);
            auto it = connectSessionsByClient_.find(clientId);
            if (it == connectSessionsByClient_.end()) return;
            oauthWizard = std::move(it->second.oauthWizard);
            apiKeyWizard = std::move(it->second.apiKeyWizard);
            providerKind = it->second.providerKind;
          }

          ConnectProgressSnapshot result;
          result.sessionId = sessionId;
          result.providerId = providerId;

          if (oauthWizard) {
            std::string err;
            if (oauthWizard->finalizeExchange(err)) {
              result.phase = ConnectProgressPhase::Succeeded;
              result.message = oauthWizard->getFinalMessage();
              firmius::core::Harness::instance().invalidateModelCache();
            } else {
              result.phase = ConnectProgressPhase::Failed;
              result.message = err.empty() ? "OAuth exchange failed." : err;
            }
          } else if (apiKeyWizard) {
            std::string apiKey, err;
            if (apiKeyWizard->finalizeExchange(apiKey, err)) {
              auto provider =
                  firmius::provider::ProviderRegistry::instance().getProvider(
                      providerId);
              auto apiKeyProvider = std::dynamic_pointer_cast<
                  firmius::provider::BaseAPIKeyProvider>(provider);
              if (apiKeyProvider && !apiKey.empty()) {
                apiKeyProvider->addApiKey(apiKey);
                firmius::core::Harness::instance().invalidateModelCache();
              }
              result.phase = ConnectProgressPhase::Succeeded;
              result.message = apiKeyWizard->getFinalMessage();
            } else {
              result.phase = ConnectProgressPhase::Failed;
              result.message = err.empty() ? "API key save failed." : err;
            }
          } else {
            result.phase = ConnectProgressPhase::Failed;
            result.message = "Wizard had already been consumed.";
          }

          emitConnectProgressEvent(clientId, result);

          // Erase the session so a new /connect can start cleanly. Detach
          // first so the std::thread destructor doesn't try to join itself.
          {
            std::lock_guard<std::mutex> lk(connectSessionsMutex_);
            auto it = connectSessionsByClient_.find(clientId);
            if (it != connectSessionsByClient_.end() &&
                it->second.sessionId == sessionId) {
              it->second.finalizeWorker.detach();
              connectSessionsByClient_.erase(it);
            }
          }
        });
  }

  response.accepted = true;
  return response;
}

ConnectCancelResponse
DaemonService::cancelConnect(const std::string &clientId,
                              const ConnectCancelRequest &request) {
  ConnectCancelResponse response;
  // Verify it's the right session before cancelling — don't let an old/stale
  // sessionId yank a brand-new wizard out from under the user.
  {
    std::lock_guard<std::mutex> lk(connectSessionsMutex_);
    auto it = connectSessionsByClient_.find(clientId);
    if (it == connectSessionsByClient_.end() ||
        it->second.sessionId != request.sessionId) {
      return response;
    }
  }
  cancelConnectSessionForClientLocked(clientId);
  response.cancelled = true;
  return response;
}

// ── /undo Rewind ────────────────────────────────────────────────────────
//
// previewRewind: walks the agent history from head until targetTurnId,
// counts turns that would be discarded, then queries listAgentEditBatches
// for batches whose turnId falls in that discarded set. Per-batch
// EditUndoEligibility is included. No state is mutated.
//
// executeRewind: delegates to Harness::compoundRewind which atomically
// pre-flights eligibilities, undoes edits in reverse, then transcript
// turns. On success, emits a DaemonEventKind::RewindApplied event to all
// subscribed clients (not just the requester) so other UIs viewing the
// thread refresh.

namespace {

std::string firstTextLine(const firmius::shared::Message &msg) {
  for (const auto &part : msg.content) {
    if (std::holds_alternative<firmius::shared::TextContent>(part)) {
      const auto &text = std::get<firmius::shared::TextContent>(part).text;
      // Take the first non-empty line, trimmed.
      auto eol = text.find('\n');
      std::string head = eol == std::string::npos ? text : text.substr(0, eol);
      while (!head.empty() &&
             std::isspace(static_cast<unsigned char>(head.front()))) {
        head.erase(head.begin());
      }
      while (!head.empty() &&
             std::isspace(static_cast<unsigned char>(head.back()))) {
        head.pop_back();
      }
      if (!head.empty()) return head;
    }
  }
  return {};
}

}  // namespace

RewindPreviewResponse
DaemonService::previewRewind(const std::string &clientId,
                              const RewindPreviewRequest &request) const {
  RewindPreviewResponse response;
  response.threadId = resolveThreadIdForRequest(clientId, request.threadId);
  response.agentId = resolveAgentIdForRequest(clientId, response.threadId,
                                              request.agentId);
  response.targetTurnId = request.targetTurnId;

  if (response.threadId.empty() || response.agentId.empty() ||
      request.targetTurnId.empty()) {
    response.errorMessage = "thread_id, agent_id, and target_turn_id are required";
    return response;
  }

  // Walk history to find the target turn and count what's after it.
  // We can't use Harness::getAgentHistoryPtr here — that helper insists
  // currentThreadId_ matches even when the caller passed a threadId
  // explicitly. previewRewind is addressable for any thread the caller
  // names (multi-client daemon), so we go through the in-memory agent
  // first and fall back to the thread manager keyed on response.threadId.
  std::shared_ptr<firmius::shared::AgentHistory> history;
  if (auto agent = firmius::core::AgentRegistry::instance().getAgent(response.agentId)) {
    auto inMemory = agent->getContext().history;
    if (inMemory && !inMemory->turns.empty()) {
      history = std::make_shared<firmius::shared::AgentHistory>(*inMemory);
    }
  }
  if (!history) {
    firmius::core::ThreadManager tm(
        firmius::core::ThreadManager::defaultBasePath());
    auto loaded = tm.loadAgentHistory(response.threadId, response.agentId);
    if (!loaded.turns.empty()) {
      history = std::make_shared<firmius::shared::AgentHistory>(std::move(loaded));
    }
  }
  if (!history || history->turns.empty()) {
    response.errorMessage = "agent has no history";
    return response;
  }
  int targetIdx = -1;
  for (int i = 0; i < static_cast<int>(history->turns.size()); ++i) {
    if (history->turns[i].turnId == request.targetTurnId) {
      targetIdx = i;
      break;
    }
  }
  if (targetIdx < 0) {
    response.errorMessage = "target turn not found";
    return response;
  }
  response.turnsToUndo =
      static_cast<int>(history->turns.size()) - targetIdx;
  if (!history->turns[targetIdx].messages.empty()) {
    response.targetMessagePreview = firstTextLine(history->turns[targetIdx].messages.front());
    response.targetMessageCreatedAt =
        history->turns[targetIdx].messages.front().timestamp;
  }

  // Find edit batches whose turnId falls in the discarded set. We use
  // includeUndone=false because already-undone batches don't need to be
  // surfaced (and would confuse the +A/-B aggregate).
  std::unordered_set<std::string> discardedTurnIds;
  for (int i = targetIdx; i < static_cast<int>(history->turns.size()); ++i) {
    discardedTurnIds.insert(history->turns[i].turnId);
  }

  firmius::shared::EditHistoryFilters filters;
  filters.agentId = response.agentId;
  filters.includeUndone = false;
  auto batches = firmius::core::Engine::instance().listAgentEditBatches(
      response.threadId, filters);

  std::unordered_set<std::string> seenFiles;
  for (const auto &batch : batches) {
    if (!discardedTurnIds.count(batch.turnId)) continue;
    response.affectedEditBatches.push_back(batch);
    response.editEligibilities.push_back(
        firmius::core::Engine::instance().evaluateEditBatchUndo(
            response.threadId, batch.editBatchId));
    response.totalAddedLines += batch.addedLines;
    response.totalRemovedLines += batch.removedLines;
    for (const auto &f : batch.files) {
      if (seenFiles.insert(f).second) {
        response.filesAffected.push_back(f);
      }
    }
  }

  // Sort newest-first so the overlay shows the most recent change at the top.
  // We sort both arrays in lockstep by re-pairing.
  std::vector<std::pair<firmius::shared::EditBatchSummary,
                        firmius::shared::EditUndoEligibility>>
      paired;
  paired.reserve(response.affectedEditBatches.size());
  for (size_t i = 0; i < response.affectedEditBatches.size(); ++i) {
    paired.emplace_back(response.affectedEditBatches[i],
                        response.editEligibilities[i]);
  }
  std::sort(paired.begin(), paired.end(),
            [](const auto &a, const auto &b) {
              return a.first.createdAt > b.first.createdAt;
            });
  response.affectedEditBatches.clear();
  response.editEligibilities.clear();
  for (auto &[b, e] : paired) {
    response.affectedEditBatches.push_back(std::move(b));
    response.editEligibilities.push_back(std::move(e));
  }

  // Aggregate "is the whole code restore safe right now?".
  for (const auto &elig : response.editEligibilities) {
    if (!elig.undoable) {
      response.codeRestoreSafe = false;
      response.codeRestoreBlockReason = elig.reason.empty()
                                             ? std::string("at least one edit is blocked")
                                             : elig.reason;
      break;
    }
  }

  return response;
}

RewindExecuteResponse
DaemonService::executeRewind(const std::string &clientId,
                              const RewindExecuteRequest &request) {
  RewindExecuteResponse response;
  std::string threadId = resolveThreadIdForRequest(clientId, request.threadId);
  std::string agentId = resolveAgentIdForRequest(clientId, threadId, request.agentId);
  if (threadId.empty() || agentId.empty() || request.targetTurnId.empty()) {
    response.errorMessage = "thread_id, agent_id, and target_turn_id are required";
    return response;
  }

  using HarnessMode = firmius::core::Harness::CompoundRewindMode;
  HarnessMode mode = HarnessMode::RestoreCodeAndConversation;
  switch (request.mode) {
  case RewindMode::RestoreCodeAndConversation:
    mode = HarnessMode::RestoreCodeAndConversation;
    break;
  case RewindMode::RestoreConversation:
    mode = HarnessMode::RestoreConversation;
    break;
  case RewindMode::RestoreCode:
    mode = HarnessMode::RestoreCode;
    break;
  }

  // Hold the runtime mutex so concurrent edits/turns don't race the rewind.
  std::lock_guard<std::mutex> runtimeLock(runtimeMutex_);
  auto result = firmius::core::Harness::instance().compoundRewind(
      threadId, agentId, request.targetTurnId, mode);

  response.applied = result.applied;
  response.undoActionId = result.undoActionId;
  response.editUndoActionIds = result.editUndoActionIds;
  response.errorMessage = result.errorMessage;

  if (response.applied) {
    // Broadcast to every client viewing this thread. We use the same
    // listener-iteration pattern as emitSessionEvent — guards against
    // listener invalidation during iteration.
    std::vector<std::pair<std::string, DaemonEventListener>> listeners;
    {
      std::lock_guard<std::mutex> lock(stateMutex_);
      for (const auto &[cid, subscription] : subscriptions_) {
        if (subscription.listener) {
          listeners.push_back({cid, subscription.listener});
        }
      }
    }
    RewindAppliedSnapshot snap;
    snap.threadId = threadId;
    snap.agentId = agentId;
    snap.targetTurnId = request.targetTurnId;
    snap.mode = request.mode;
    snap.turnsUndone = result.turnsUndone;
    snap.editBatchesUndone = static_cast<int>(result.editUndoActionIds.size());
    snap.undoActionId = result.undoActionId;
    DaemonEventEnvelope envelope;
    envelope.kind = DaemonEventKind::RewindApplied;
    envelope.rewindApplied = snap;
    envelope = prepareEventEnvelope(std::move(envelope));
    for (auto &[cid, listener] : listeners) {
      (void)cid;
      listener(envelope);
    }
  }

  return response;
}

RedoPreviewResponse
DaemonService::previewRedo(const std::string &clientId,
                           const RedoPreviewRequest &request) const {
  RedoPreviewResponse response;
  response.threadId = resolveThreadIdForRequest(clientId, request.threadId);
  response.agentId = resolveAgentIdForRequest(clientId, response.threadId,
                                              request.agentId);
  if (response.threadId.empty() || response.agentId.empty()) {
    response.errorMessage = "thread_id and agent_id are required";
    return response;
  }

  firmius::core::ThreadManager tm(
      firmius::core::ThreadManager::defaultBasePath());
  const int limit = request.limit > 0 ? request.limit : 10;
  auto undoActions = tm.listTranscriptUndoActions(response.threadId, limit);
  // ThreadManager returns newest-first already, but be defensive — the
  // redo picker only makes sense if the order is right.
  std::sort(undoActions.begin(), undoActions.end(),
            [](const auto &a, const auto &b) {
              return a.createdAt > b.createdAt;
            });

  for (const auto &action : undoActions) {
    if (response.actions.size() >= static_cast<size_t>(limit)) break;
    RedoUndoActionSummary summary;
    summary.undoActionId = action.undoActionId;
    summary.createdAt = action.createdAt;
    summary.editBatchesToRedo =
        static_cast<int>(action.editUndoActionIds.size());
    summary.redoAvailable = action.redoAvailable;

    // Load the captured payload so we can show a 1-line preview of what
    // would come back. We deliberately ignore load errors here — the
    // overlay can still pick the action by id, the preview is just nice-to-have.
    try {
      auto payloads = tm.loadTranscriptRedoPayloads(response.threadId,
                                                    action.undoActionId);
      int totalTurns = 0;
      for (const auto &payload : payloads) {
        totalTurns += static_cast<int>(payload.turns.size());
      }
      summary.turnsToRedo = totalTurns;
      // First captured turn = first message text. If there is none we
      // leave the preview blank rather than emit a fake one.
      for (const auto &payload : payloads) {
        if (!payload.turns.empty() &&
            !payload.turns.front().messages.empty()) {
          summary.firstTurnPreview =
              firstTextLine(payload.turns.front().messages.front());
          break;
        }
      }
    } catch (...) {
      // Best-effort preview only.
    }
    response.actions.push_back(std::move(summary));
  }

  return response;
}

RedoExecuteResponse
DaemonService::executeRedo(const std::string &clientId,
                           const RedoExecuteRequest &request) {
  RedoExecuteResponse response;
  std::string threadId = resolveThreadIdForRequest(clientId, request.threadId);
  std::string agentId = resolveAgentIdForRequest(clientId, threadId, request.agentId);
  if (threadId.empty() || agentId.empty() || request.undoActionId.empty()) {
    response.errorMessage =
        "thread_id, agent_id, and undo_action_id are required";
    return response;
  }

  using HarnessMode = firmius::core::Harness::CompoundRedoMode;
  HarnessMode mode = HarnessMode::RestoreCodeAndConversation;
  switch (request.mode) {
  case RedoMode::RestoreCodeAndConversation:
    mode = HarnessMode::RestoreCodeAndConversation;
    break;
  case RedoMode::RestoreConversation:
    mode = HarnessMode::RestoreConversation;
    break;
  case RedoMode::RestoreCode:
    mode = HarnessMode::RestoreCode;
    break;
  }

  std::lock_guard<std::mutex> runtimeLock(runtimeMutex_);
  auto result = firmius::core::Harness::instance().compoundRedo(
      threadId, agentId, request.undoActionId, mode);

  response.applied = result.applied;
  response.turnsRedone = result.turnsRedone;
  response.editRedoActionIds = result.editRedoActionIds;
  response.errorMessage = result.errorMessage;

  if (response.applied) {
    // Reuse RewindApplied for the broadcast — clients refresh on the
    // same hook. The mode field doesn't have a "redo" variant, so we
    // pass the request's redo mode coerced into the rewind enum
    // (matching wire format), and the snapshot's targetTurnId is empty
    // since this is a redo, not a rewind. The TUI distinguishes
    // refresh-only from picker-replay using turnsUndone < 0; here
    // turnsUndone=0 is a forward-only signal.
    std::vector<std::pair<std::string, DaemonEventListener>> listeners;
    {
      std::lock_guard<std::mutex> lock(stateMutex_);
      for (const auto &[cid, subscription] : subscriptions_) {
        if (subscription.listener) {
          listeners.push_back({cid, subscription.listener});
        }
      }
    }
    RewindAppliedSnapshot snap;
    snap.threadId = threadId;
    snap.agentId = agentId;
    snap.undoActionId = request.undoActionId;
    snap.turnsUndone = -result.turnsRedone;  // negative = redo
    snap.editBatchesUndone = -static_cast<int>(result.editRedoActionIds.size());
    DaemonEventEnvelope envelope;
    envelope.kind = DaemonEventKind::RewindApplied;
    envelope.rewindApplied = snap;
    envelope = prepareEventEnvelope(std::move(envelope));
    for (auto &[cid, listener] : listeners) {
      (void)cid;
      listener(envelope);
    }
  }

  return response;
}

} // namespace firmius::daemon

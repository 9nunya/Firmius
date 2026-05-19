#include "EventRouter.hpp"
#include "Serialization.hpp"
#include "items/SimpleItems.hpp"
#include "items/StreamingItems.hpp"
#include "items/QuickToolClusterItem.hpp"
#include "items/ToolCallItem.hpp"
#include "Icons.hpp"
#include "utils/ToolSummaries.hpp"

#include <rapidjson/document.h>
#include <algorithm>
#include <chrono>
#include <sstream>

namespace {

using firmius::tui::ToolPhase;
using SharedToolPhase = firmius::shared::ToolPhase;

std::string humanizeTokens(uint32_t value) {
  if (value >= 1'000'000) {
    return std::to_string(value / 1'000'000) + "M";
  }
  if (value >= 1'000) {
    return std::to_string(value / 1'000) + "k";
  }
  return std::to_string(value);
}

/// Compute deltas vs `prev` and, if any working-memory activity actually
/// happened on this turn (deflations fired, eviction/recall changed, or
/// threshold crossed up), return a single-line notice string. Otherwise
/// return empty.
std::string formatMemoryActivityNotice(
    const firmius::tui::MemoryStatus& prev,
    const firmius::tui::MemoryStatus& cur) {
  if (!cur.aboveBufferThreshold) {
    return ""; // pure pass-through: nothing to report
  }
  const auto deltaDeflated =
      cur.deflatedPartCount > prev.deflatedPartCount
          ? cur.deflatedPartCount - prev.deflatedPartCount
          : 0u;
  const auto deltaSavedDefl =
      cur.tokensSavedByDeflation > prev.tokensSavedByDeflation
          ? cur.tokensSavedByDeflation - prev.tokensSavedByDeflation
          : 0u;
  const auto deltaSavedEvict =
      cur.tokensSavedByEviction > prev.tokensSavedByEviction
          ? cur.tokensSavedByEviction - prev.tokensSavedByEviction
          : 0u;
  const auto deltaRecalled =
      cur.recalledTurnCount > prev.recalledTurnCount
          ? cur.recalledTurnCount - prev.recalledTurnCount
          : 0u;
  const auto deltaEvicted =
      cur.evictedTurnCount > prev.evictedTurnCount
          ? cur.evictedTurnCount - prev.evictedTurnCount
          : 0u;
  const bool crossedTarget =
      cur.aboveTargetThreshold && !prev.aboveTargetThreshold;
  const bool crossedEmerg =
      cur.aboveEmergencyThreshold && !prev.aboveEmergencyThreshold;
  const bool crossedBuffer =
      cur.aboveBufferThreshold && !prev.aboveBufferThreshold;
  const bool anyChange = deltaDeflated || deltaSavedDefl || deltaSavedEvict ||
                         deltaRecalled || deltaEvicted || crossedTarget ||
                         crossedEmerg || crossedBuffer;
  if (!anyChange) {
    return "";
  }

  using firmius::shared::ICON_DEFLATE;
  using firmius::shared::ICON_EVICT;
  using firmius::shared::ICON_MEMORY;
  using firmius::shared::ICON_RECALL;
  using firmius::shared::ICON_SAVINGS;
  using firmius::shared::ICON_THRESHOLD_BUF;
  using firmius::shared::ICON_THRESHOLD_EMERG;
  using firmius::shared::ICON_THRESHOLD_TGT;

  std::ostringstream out;
  out << ICON_MEMORY << "  ";

  if (crossedEmerg) {
    out << ICON_THRESHOLD_EMERG << " emergency  ";
  } else if (crossedTarget) {
    out << ICON_THRESHOLD_TGT << " target  ";
  } else if (crossedBuffer) {
    out << ICON_THRESHOLD_BUF << " buffer  ";
  }

  bool first = true;
  auto sep = [&]() {
    if (first) {
      first = false;
    } else {
      out << "  · ";
    }
  };

  if (deltaDeflated > 0) {
    sep();
    out << ICON_DEFLATE << " " << deltaDeflated;
  }
  if (deltaEvicted > 0) {
    sep();
    out << ICON_EVICT << " " << deltaEvicted;
  }
  if (deltaRecalled > 0) {
    sep();
    out << ICON_RECALL << " " << deltaRecalled;
  }
  const uint32_t savedThisTurn = deltaSavedDefl + deltaSavedEvict;
  if (savedThisTurn > 0) {
    sep();
    out << ICON_SAVINGS << " " << humanizeTokens(savedThisTurn);
  }
  if (first) {
    // Nothing concrete to report beyond the threshold crossing alone.
    return out.str();
  }
  return out.str();
}

void markThinkingDone(firmius::tui::AppState& state, const std::string& agentId) {
  state.upsertAgentActivity(agentId, "thinking", "Thought",
                            std::chrono::milliseconds(2300));
}

bool isQuickToolName(const std::string& toolName) {
  return toolName == "Read" || toolName == "Glob" || toolName == "Grep" ||
         toolName == "List";
}

bool isDelegateWaitForAgent(const firmius::tui::ToolCallState& tc,
                            const std::string& watchedAgentId) {
  if (tc.toolName != "Delegate" || tc.args.empty() || watchedAgentId.empty()) {
    return false;
  }
  rapidjson::Document doc;
  doc.Parse(tc.args.c_str());
  if (doc.HasParseError() || !doc.IsObject()) {
    return false;
  }
  if (!doc.HasMember("action") || !doc["action"].IsString() ||
      std::string(doc["action"].GetString()) != "Wait") {
    return false;
  }
  if (!doc.HasMember("agent_id") || !doc["agent_id"].IsString()) {
    return false;
  }
  return std::string(doc["agent_id"].GetString()) == watchedAgentId;
}

std::string waitResultJson(const std::string& agentId, bool success,
                           const std::string& message) {
  auto quote = [](const std::string& s) {
    std::string out = "\"";
    for (char c : s) {
      switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default: out += c; break;
      }
    }
    out += "\"";
    return out;
  };
  return std::string("{\"agentId\":") + quote(agentId) +
         ",\"status\":" + quote(success ? "completed" : "failed") +
         ",\"result\":" + quote(message) + "}";
}

void finalizeDelegateWaitsForAgent(firmius::tui::AppState& state,
                                   const std::string& watchedAgentId,
                                   bool success,
                                   const std::string& message) {
  for (auto& [id, tc] : state.toolCallsMut()) {
    if (tc.isFinished() || !isDelegateWaitForAgent(tc, watchedAgentId)) {
      continue;
    }
    tc.phase = success ? firmius::tui::ToolCallPhase::FinishedOk
                       : firmius::tui::ToolCallPhase::FinishedErr;
    tc.success = success;
    tc.result = waitResultJson(watchedAgentId, success, message);
    tc.finishedAt = std::chrono::steady_clock::now();
    if (auto* item = state.findToolCallById(id)) {
      item->setResult(success, tc.result);
      item->setLive(false);
    }
  }
}

void bindSpawnedChildToDelegate(firmius::tui::AppState& state,
                                const std::string& parentAgentId,
                                const std::string& childAgentId) {
  for (auto& [id, tc] : state.toolCallsMut()) {
    if (tc.isFinished() || tc.agentId != parentAgentId ||
        tc.toolName != "Delegate" || !tc.subagentId.empty()) {
      continue;
    }
    rapidjson::Document doc;
    doc.Parse(tc.args.c_str());
    if (doc.HasParseError() || !doc.IsObject()) {
      continue;
    }
    if (!doc.HasMember("action") || !doc["action"].IsString() ||
        std::string(doc["action"].GetString()) != "Spawn") {
      continue;
    }
    tc.subagentId = childAgentId;
    if (auto* item = state.findToolCallById(id)) {
      item->setSubagentId(childAgentId);
      item->setLive(true);
    }
  }
}

firmius::tui::MemoryStatus memoryStatusFromMetrics(
    const firmius::shared::MemoryMetrics& m) {
  firmius::tui::MemoryStatus s;
  s.rawHistoryTokens = m.rawHistoryTokens;
  s.workingSetTokens = m.workingSetTokens;
  s.pinnedTurnCount = m.pinnedTurnCount;
  s.evictedTurnCount = m.evictedTurnCount;
  s.recalledTurnCount = m.recalledTurnCount;
  s.deflatedPartCount = m.deflatedPartCount;
  s.tokensSavedByDeflation = m.tokensSavedByDeflation;
  s.tokensSavedByEviction = m.tokensSavedByEviction;
  s.tokensSpentOnSummaries = m.tokensSpentOnSummaries;
  s.tokensSpentOnEmbeddings = m.tokensSpentOnEmbeddings;
  s.hotPathLatencyMicros = m.hotPathLatencyMicros;
  s.aboveBufferThreshold = m.aboveBufferThreshold;
  s.aboveTargetThreshold = m.aboveTargetThreshold;
  s.aboveEmergencyThreshold = m.aboveEmergencyThreshold;
  s.valid = true;
  return s;
}

firmius::tui::EmbeddingDownloadState embeddingDownloadStateFromProgress(
    const rapidjson::Document& doc) {
  firmius::tui::EmbeddingDownloadState ds;
  ds.downloading = true;
  if (doc.HasMember("modelId")) ds.modelId = doc["modelId"].GetString();
  if (doc.HasMember("bytesDownloaded")) ds.bytesDownloaded = doc["bytesDownloaded"].GetUint64();
  if (doc.HasMember("totalBytes")) ds.totalBytes = doc["totalBytes"].GetUint64();
  if (doc.HasMember("status")) {
    ds.status = doc["status"].GetString();
    if (ds.status == "ready" || ds.status == "error") {
      ds.downloading = false;
    }
  }
  return ds;
}

firmius::tui::PendingPermissionSuggestion pendingPermissionSuggestionFromJson(
    const rapidjson::Value& s) {
  firmius::tui::PendingPermissionSuggestion sug;
  if (s.HasMember("ruleId") && s["ruleId"].IsString())
    sug.ruleId = s["ruleId"].GetString();
  if (s.HasMember("label") && s["label"].IsString())
    sug.label = s["label"].GetString();
  if (s.HasMember("explanation") && s["explanation"].IsString())
    sug.explanation = s["explanation"].GetString();
  if (s.HasMember("category") && s["category"].IsString())
    sug.category = s["category"].GetString();
  if (s.HasMember("decision") && s["decision"].IsString())
    sug.decision = s["decision"].GetString();
  if (s.HasMember("scope") && s["scope"].IsString())
    sug.scope = s["scope"].GetString();
  if (s.HasMember("defaultSelected") && s["defaultSelected"].IsBool())
    sug.defaultSelected = s["defaultSelected"].GetBool();
  if (s.HasMember("match") && s["match"].IsObject()) {
    for (auto it = s["match"].MemberBegin();
         it != s["match"].MemberEnd(); ++it) {
      if (it->value.IsString()) {
        sug.match[it->name.GetString()] = it->value.GetString();
      }
    }
  }
  return sug;
}

std::string jStr(const rapidjson::Document& doc, const char* field) {
  if (doc.HasMember(field) && doc[field].IsString()) return doc[field].GetString();
  return "";
}

firmius::tui::PendingPermission pendingPermissionFromJson(
    const rapidjson::Document& doc) {
  firmius::tui::PendingPermission perm;
  perm.requestId = jStr(doc, "requestId");
  perm.title = jStr(doc, "title");
  perm.message = jStr(doc, "message");
  perm.toolName = jStr(doc, "toolName");
  perm.toolCallId = jStr(doc, "toolCallId");
  if (doc.HasMember("allowAlways") && doc["allowAlways"].IsBool()) {
    perm.allowAlways = doc["allowAlways"].GetBool();
  }
  perm.category = jStr(doc, "category");
  perm.command = jStr(doc, "command");
  perm.commandPrimary = jStr(doc, "commandPrimary");
  perm.targetPath = jStr(doc, "targetPath");
  perm.cwd = jStr(doc, "cwd");
  perm.url = jStr(doc, "url");
  perm.host = jStr(doc, "host");
  perm.scheme = jStr(doc, "scheme");
  perm.query = jStr(doc, "query");
  perm.persona = jStr(doc, "persona");
  perm.parentPersona = jStr(doc, "parentPersona");
  if (doc.HasMember("severity") && doc["severity"].IsInt()) {
    perm.severity = doc["severity"].GetInt();
  }
  if (doc.HasMember("isDirectory") && doc["isDirectory"].IsBool()) {
    perm.isDirectory = doc["isDirectory"].GetBool();
  }
  if (doc.HasMember("subcommands") && doc["subcommands"].IsArray()) {
    for (const auto &s : doc["subcommands"].GetArray()) {
      if (s.IsString()) perm.subcommands.emplace_back(s.GetString());
    }
  }
  if (doc.HasMember("suggestions") && doc["suggestions"].IsArray()) {
    for (const auto &s : doc["suggestions"].GetArray()) {
      if (!s.IsObject()) continue;
      perm.suggestions.push_back(pendingPermissionSuggestionFromJson(s));
    }
  }
  return perm;
}

} // namespace

#include "utils/ToolView.hpp"

#include <cstdio>
#include <rapidjson/document.h>

namespace firmius::tui {

EventRouter::EventRouter(AppState &state) : state_(state) {}

void EventRouter::route(const firmius::daemon::DaemonEventEnvelope &envelope) {
  switch (envelope.kind) {
  case firmius::daemon::DaemonEventKind::RuntimeAppEvent:
    routeRuntimeEvent(envelope.runtimeEventType, envelope.runtimeEventJson,
                      envelope.runtimeEventThreadId, envelope.runtimeEventAgentId,
                      envelope.agentStatus);
    break;
  case firmius::daemon::DaemonEventKind::ClientSessionRegistered:
  case firmius::daemon::DaemonEventKind::ClientSessionDisconnected:
  case firmius::daemon::DaemonEventKind::ClientSessionUpdated:
    break;
  case firmius::daemon::DaemonEventKind::HookStateChanged:
    if (envelope.hookState.has_value()) {
      handleHookStateChanged(*envelope.hookState);
    }
    break;
  case firmius::daemon::DaemonEventKind::InitProgress:
    if (!envelope.initMessage.empty()) {
      state_.setLiveMessage(envelope.initMessage);
    }
    break;
  case firmius::daemon::DaemonEventKind::ConnectProgress:
    if (envelope.connectProgress.has_value() && onConnectProgress_) {
      onConnectProgress_(*envelope.connectProgress);
    }
    break;
  case firmius::daemon::DaemonEventKind::RewindApplied:
    if (envelope.rewindApplied.has_value() && onRewindApplied_) {
      onRewindApplied_(*envelope.rewindApplied);
    }
    break;
  }
}

void EventRouter::routeRuntimeEvent(
    const std::string &eventType, const std::string &eventJson,
    const std::string & /*threadId*/, const std::string &agentId,
    std::optional<firmius::shared::AgentStatus> realStatus) {
  if (eventType == "agent_text") {
    handleAgentText(eventJson, agentId);
  } else if (eventType == "agent_thinking") {
    handleAgentThinking(eventJson, agentId);
  } else if (eventType == "agent_tool_call_chunk") {
    handleAgentToolCallChunk(eventJson, agentId);
  } else if (eventType == "agent_tool_call") {
    handleAgentToolCall(eventJson, agentId);
  } else if (eventType == "agent_file_edited") {
    handleAgentFileEdited(eventJson);
  } else if (eventType == "agent_process_spawned") {
    handleAgentProcessSpawned(eventJson, agentId);
  } else if (eventType == "agent_process_output") {
    handleAgentProcessOutput(eventJson, agentId);
  } else if (eventType == "agent_turn_completed") {
    handleAgentTurnCompleted(eventJson, agentId);
  } else if (eventType == "agent_finished") {
    handleAgentFinished(agentId);
  } else if (eventType == "agent_spawned") {
    handleAgentSpawned(eventJson, agentId);
  } else if (eventType == "agent_error") {
    handleAgentError(eventJson, agentId);
  } else if (eventType == "agent_interrupted") {
    handleAgentInterrupted(agentId);
  } else if (eventType == "user_message_sent") {
    handleUserMessageSent(eventJson);
  } else if (eventType == "message_queued") {
    handleMessageQueued(eventJson);
  } else if (eventType == "message_dequeued") {
    handleMessageDequeued(eventJson);
  } else if (eventType == "permission_escalation_request") {
    handlePermissionEscalation(eventJson);
  } else if (eventType == "permission_escalation_resolved") {
    handlePermissionResolved(eventJson);
  } else if (eventType == "agent_todo_updated") {
    handleAgentTodoUpdated(eventJson);
  } else if (eventType == "model_switched") {
    handleModelSwitched(eventJson);
  } else if (eventType == "thread_title_updated") {
    handleThreadTitleUpdated(eventJson);
  } else if (eventType == "thread_metadata_updated") {
    handleThreadMetadataUpdated(eventJson);
  } else if (eventType == "config_updated") {
    handleConfigUpdated();
  } else if (eventType == "embedding_model_progress") {
    handleEmbeddingModelProgress(eventJson);
  } else if (eventType == "agent_compacting") {
    handleAgentCompacting(agentId);
  } else if (eventType == "context_compacted") {
    handleContextCompacted(eventJson, agentId);
  }

  if (realStatus.has_value()) {
    // Update per-agent status
    auto* agent = state_.findAgentState(agentId);
    if (agent) agent->status = *realStatus;
    // Only update global status if this is the focused agent
    if (agentId == state_.focusedAgentId()) {
      state_.setAgentStatus(*realStatus);
    }
  }
}

namespace {

std::string jsonString(const rapidjson::Document &doc, const char *field) {
  if (doc.HasMember(field) && doc[field].IsString()) {
    return doc[field].GetString();
  }
  return "";
}

int jsonInt(const rapidjson::Document &doc, const char *field, int def = 0) {
  if (doc.HasMember(field) && doc[field].IsInt()) return doc[field].GetInt();
  return def;
}

// Escape a string for JSON embedding.
std::string escapeJson(const std::string& s) {
  std::string out = "\"";
  for (char c : s) {
    switch (c) {
      case '"':  out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:   out += c; break;
    }
  }
  out += "\"";
  return out;
}

std::optional<size_t> firstCurrentTurnToolIndex(const AppState& state,
                                                const std::string& agentId) {
  const auto* agent = state.findAgentState(agentId);
  if (!agent || !agent->currentTurn.has_value()) {
    return std::nullopt;
  }

  std::optional<size_t> firstIndex;
  const auto& items = state.items();
  for (const auto& toolCallId : agent->currentTurn->toolCallIds) {
    for (size_t i = 0; i < items.size(); ++i) {
      if (items[i]->type() != "ToolCall") {
        continue;
      }
      const auto* tool =
          static_cast<const ToolCallItem*>(items[i].get());
      if (tool->agentId() == agentId && tool->toolCallId() == toolCallId) {
        firstIndex = firstIndex.has_value() ? std::min(*firstIndex, i) : i;
        break;
      }
    }
  }
  return firstIndex;
}

} // namespace

// ── Streaming text/thinking (unchanged) ──

void EventRouter::handleAgentText(const std::string &json,
                                   const std::string &agentId) {
  rapidjson::Document doc;
  doc.Parse(json.c_str());
  if (doc.HasParseError()) return;

  std::string delta = jsonString(doc, "delta");
  if (delta.empty()) return;

  const std::string focusedId = state_.focusedAgentId();
  bool isFocused = focusedId.empty() || agentId == focusedId;

  // Finalize this agent's thinking item (text replaces thinking)
  auto* thinkItem = state_.agentThinkingItem(agentId);
  if (thinkItem && !thinkItem->isFinalized()) {
    thinkItem->finalize();
    state_.setAgentThinkingItem(agentId, nullptr);
    if (isFocused) state_.setActiveThinkingItem(nullptr);
  }
  markThinkingDone(state_, agentId);
  markThinkingDone(state_, agentId);
  markThinkingDone(state_, agentId);

  // Quick-tool clusters should span exploratory turns and collapse only once
  // the agent starts producing the actual answer text.
  if (auto* cluster = state_.agentQuickToolClusterItem(agentId)) {
    if (!cluster->isFinalized()) {
      cluster->finalize();
    }
    state_.setAgentQuickToolClusterItem(agentId, nullptr);
  }

  // Find or create a text item for THIS agent
  auto* item = state_.agentTextItem(agentId);
  if (!item || item->isFinalized()) {
    auto newItem = std::make_unique<AgentTextItem>();
    newItem->setAgentId(agentId);
    item = newItem.get();
    if (auto insertIndex = firstCurrentTurnToolIndex(state_, agentId);
        insertIndex.has_value()) {
      state_.insertItem(*insertIndex, std::move(newItem));
    } else {
      state_.addItem(std::move(newItem));
    }
    state_.setAgentTextItem(agentId, item);
    if (isFocused) state_.setActiveTextItem(item);
  }
  item->appendDelta(delta);
  state_.upsertAgentActivity(agentId, "thinking", "Thinking...");
  if (isFocused) {
    state_.markDirtyPublic();
  }
}

void EventRouter::handleAgentThinking(const std::string &json,
                                       const std::string &agentId) {
  rapidjson::Document doc;
  doc.Parse(json.c_str());
  if (doc.HasParseError()) return;

  std::string delta = jsonString(doc, "delta");
  if (delta.empty()) return;

  const std::string focusedId = state_.focusedAgentId();
  bool isFocused = focusedId.empty() || agentId == focusedId;

  // A fresh thinking block starts a new exploration/answer phase, so any
  // previous quick-tools cluster should stay attached to the transcript
  // section before this thinking item.
  if (auto* cluster = state_.agentQuickToolClusterItem(agentId)) {
    if (!cluster->isFinalized()) {
      cluster->finalize();
    }
    state_.setAgentQuickToolClusterItem(agentId, nullptr);
  }

  // Find or create a thinking item for THIS agent
  auto* item = state_.agentThinkingItem(agentId);
  if (!item || item->isFinalized()) {
    auto newItem = std::make_unique<AgentThinkingItem>();
    newItem->setAgentId(agentId);
    item = newItem.get();
    state_.addItem(std::move(newItem));
    state_.setAgentThinkingItem(agentId, item);
    if (isFocused) state_.setActiveThinkingItem(item);
  }
  item->appendDelta(delta);
  if (isFocused) {
    state_.markDirtyPublic();
  }
}

// ── Tool Call Lifecycle ──

void EventRouter::handleAgentToolCallChunk(const std::string &json,
                                            const std::string &agentId) {
  rapidjson::Document doc;
  doc.Parse(json.c_str());
  if (doc.HasParseError()) return;

  std::string toolCallId = jsonString(doc, "toolCallId");
  if (toolCallId.empty()) return;

  // Update ToolCallState
  auto& tc = state_.getOrCreateToolCall(toolCallId);
  if (tc.toolCallId.empty()) {
    // First chunk — initialize
    tc.toolCallId = toolCallId;
    tc.agentId = agentId;
    tc.phase = ToolCallPhase::Streaming;
  }

  // Accumulate name and args deltas
  std::string nameDelta = jsonString(doc, "nameDelta");
  std::string argsDelta = jsonString(doc, "argsDelta");
  if (!nameDelta.empty()) tc.nameAccum += nameDelta;
  if (!argsDelta.empty()) tc.argsAccum += argsDelta;

  // Update tool name from accumulated chunks
  if (!tc.nameAccum.empty()) tc.toolName = tc.nameAccum;
  if (!tc.toolName.empty()) {
    state_.upsertAgentActivity(
        agentId, "tool:" + toolCallId,
        firmius::shared::SummarizeToolCall(tc.toolName, tc.argsAccum,
                                           SharedToolPhase::Preparing));
  }

  if (isQuickToolName(tc.toolName)) {
    auto* cluster = state_.agentQuickToolClusterItem(agentId);
    if (!cluster || cluster->isFinalized()) {
      auto newItem = std::make_unique<QuickToolClusterItem>();
      newItem->setAgentId(agentId);
      cluster = newItem.get();
      state_.addItem(std::move(newItem));
      state_.setAgentQuickToolClusterItem(agentId, cluster);
    }
    cluster->addOrUpdateCall(toolCallId, tc.toolName, tc.argsAccum, true);
    state_.markDirtyPublic();
    return;
  }

  // Find or create the ToolCallItem in the transcript
  auto* item = state_.findToolCallById(toolCallId);
  if (!item) {
    auto newItem = std::make_unique<ToolCallItem>(toolCallId, tc.toolName, agentId);
    newItem->setAppState(&state_);
    newItem->setLive(true);
    item = newItem.get();
    state_.addItem(std::move(newItem));
  }

  // Update name on the item
  if (!tc.toolName.empty()) {
    // The item's name is set at construction; we can update via setArgs with partial
    // For now, the item name is set once from the first chunk
  }

  state_.markDirtyPublic();
}

void EventRouter::handleAgentToolCall(const std::string &json,
                                       const std::string &agentId) {
  rapidjson::Document doc;
  doc.Parse(json.c_str());
  if (doc.HasParseError()) return;

  std::string toolCallId = jsonString(doc, "toolCallId");
  std::string toolName = jsonString(doc, "toolName");
  std::string toolArgs = jsonString(doc, "toolArgs");

  if (toolCallId.empty() || toolName.empty()) return;

  // Finalize this agent's streaming items.
  auto* textItem = state_.agentTextItem(agentId);
  if (textItem && !textItem->isFinalized()) {
    textItem->finalize();
    state_.setAgentTextItem(agentId, nullptr);
    if (state_.focusedAgentId().empty() || state_.focusedAgentId() == agentId) {
      state_.setActiveTextItem(nullptr);
    }
  }
  auto* thinkItem = state_.agentThinkingItem(agentId);
  if (thinkItem && !thinkItem->isFinalized()) {
    thinkItem->finalize();
    state_.setAgentThinkingItem(agentId, nullptr);
    if (state_.focusedAgentId().empty() || state_.focusedAgentId() == agentId) {
      state_.setActiveThinkingItem(nullptr);
    }
  }
  markThinkingDone(state_, agentId);

  // Update ToolCallState
  auto& tc = state_.getOrCreateToolCall(toolCallId);
  tc.toolCallId = toolCallId;
  tc.toolName = toolName;
  tc.agentId = agentId;
  tc.args = toolArgs;
  tc.phase = ToolCallPhase::Prepared;
  tc.calledAt = std::chrono::steady_clock::now();

  // Track in agent's current turn
  auto& agent = state_.getOrCreateAgent(agentId);
  if (agent.currentTurn.has_value()) {
    agent.currentTurn->toolCallIds.push_back(toolCallId);
  }

  if (isQuickToolName(toolName)) {
    auto* cluster = state_.agentQuickToolClusterItem(agentId);
    if (!cluster || cluster->isFinalized()) {
      auto newItem = std::make_unique<QuickToolClusterItem>();
      newItem->setAgentId(agentId);
      cluster = newItem.get();
      state_.addItem(std::move(newItem));
      state_.setAgentQuickToolClusterItem(agentId, cluster);
    }
    cluster->addOrUpdateCall(toolCallId, toolName, toolArgs, true);
    state_.markDirtyPublic();
    return;
  }

  // A non-quick tool breaks the quick-tools exploration cluster.
  if (auto* cluster = state_.agentQuickToolClusterItem(agentId)) {
    if (!cluster->isFinalized()) {
      cluster->finalize();
    }
    state_.setAgentQuickToolClusterItem(agentId, nullptr);
  }

  auto* existingItem = state_.findToolCallById(toolCallId);
  if (existingItem) {
    existingItem->setArgs(toolArgs);
    existingItem->setPhase(ToolPhase::Called);
    state_.upsertAgentActivity(
        agentId, "tool:" + toolCallId,
        firmius::shared::SummarizeToolCall(toolName, toolArgs,
                                           SharedToolPhase::Called));
    existingItem->setLive(true);
  } else {
    auto item = std::make_unique<ToolCallItem>(toolCallId, toolName, agentId);
    item->setAppState(&state_);
    item->setArgs(toolArgs);
    item->setPhase(ToolPhase::Called);
    state_.upsertAgentActivity(
        agentId, "tool:" + toolCallId,
        firmius::shared::SummarizeToolCall(toolName, toolArgs,
                                           SharedToolPhase::Called));
    item->setLive(true);
    state_.addItem(std::move(item));
  }

  state_.markDirtyPublic();
}

void EventRouter::handleAgentFileEdited(const std::string &json) {
  rapidjson::Document doc;
  doc.Parse(json.c_str());
  if (doc.HasParseError()) return;

  std::string toolCallId = jsonString(doc, "toolCallId");
  if (toolCallId.empty()) return;

  firmius::shared::FileEditSignal signal;
  signal.path = jsonString(doc, "path");
  signal.diffPreview = jsonString(doc, "diffPreview");
  signal.addedLines = jsonInt(doc, "addedLines");
  signal.removedLines = jsonInt(doc, "removedLines");

  auto* item = state_.findToolCallById(toolCallId);
  if (item) {
    item->addDiffEdit(std::move(signal));
    state_.markDirtyPublic();
  }
}

// ── Process Tracking ──

void EventRouter::handleAgentProcessSpawned(const std::string &json,
                                             const std::string &agentId) {
  rapidjson::Document doc;
  doc.Parse(json.c_str());
  if (doc.HasParseError()) return;

  std::string processId = jsonString(doc, "processId");
  std::string toolCallId = jsonString(doc, "toolCallId");

  if (processId.empty() || toolCallId.empty()) return;

  // Map processId → toolCallId
  state_.mapProcessToTool(processId, toolCallId);

  // Update ToolCallState
  auto* tc = state_.findToolCallState(toolCallId);
  if (tc) {
    tc->processId = processId;
    tc->phase = ToolCallPhase::Executing;
  }

  // Update ToolCallItem
  auto* item = state_.findToolCallById(toolCallId);
  if (item) {
    item->setProcessId(processId);
    item->setPhase(ToolPhase::Called);
    item->setLive(true);
    state_.upsertAgentActivity(
        agentId, "tool:" + toolCallId,
        firmius::shared::SummarizeToolCall(item->toolName(), item->args(),
                                           SharedToolPhase::Called));
  }

  // Flush any process output that arrived before this mapping existed
  flushPendingProcessOutput(processId);

  state_.markDirtyPublic();
}

void EventRouter::handleAgentProcessOutput(const std::string &json,
                                            const std::string & /*agentId*/) {
  rapidjson::Document doc;
  doc.Parse(json.c_str());
  if (doc.HasParseError()) return;

  std::string processId = jsonString(doc, "processId");
  std::string output = jsonString(doc, "output");
  bool isStderr = doc.HasMember("isStderr") && doc["isStderr"].IsBool() &&
                  doc["isStderr"].GetBool();
  bool finished = doc.HasMember("finished") && doc["finished"].IsBool() &&
                  doc["finished"].GetBool();
  int exitCode = jsonInt(doc, "exitCode", 0);
  double durationMs = 0.0;
  if (doc.HasMember("durationMs") && doc["durationMs"].IsNumber()) {
    durationMs = doc["durationMs"].IsDouble()
        ? doc["durationMs"].GetDouble()
        : static_cast<double>(doc["durationMs"].GetInt());
  }

  if (processId.empty()) return;

  // Find the tool call via processId mapping
  std::string toolCallId = state_.findToolCallByProcessId(processId);
  ToolCallItem* toolItem = toolCallId.empty() ? nullptr : state_.findToolCallById(toolCallId);

  if (toolItem) {
    // Accumulate output into the ToolCallItem
    if (!output.empty()) {
      toolItem->appendProcessOutput(output, isStderr);
    }

    // Also update ToolCallState
    auto* tc = state_.findToolCallState(toolCallId);
    if (tc) {
      if (isStderr) tc->processStderr += output;
      else tc->processStdout += output;
    }

    if (finished) {
      // Track exit info on the ToolCallState — but do NOT call setResult().
      // The real result comes from handleAgentTurnCompleted which has the
      // authoritative ToolResultContent from the engine.
      if (tc) {
        tc->processExitCode = exitCode;
        tc->processDurationMs = durationMs;
        tc->processFinished = true;
      }
      // Store exit info on the item so presenters can show it,
      // but keep the item live — finalization happens in handleAgentTurnCompleted.
      toolItem->setProcessExitInfo(exitCode, durationMs);
    }

    state_.markDirtyPublic();
  } else if (!output.empty()) {
    // No matching tool call yet — buffer for replay when mapping arrives
    auto& buf = pendingProcessOutput_[processId];
    buf.push_back({output, isStderr, finished, exitCode, durationMs});
    // Cap orphan event accumulation
    constexpr std::size_t kMaxPendingEvents = 200;
    if (buf.size() > kMaxPendingEvents) {
      buf.erase(buf.begin(), buf.begin() + (buf.size() - kMaxPendingEvents));
    }
  }
}

// ── Turn / Agent Lifecycle ──

void EventRouter::handleAgentTurnCompleted(const std::string &json,
                                           const std::string &agentId) {
  const std::string focusedId = state_.focusedAgentId();
  bool isFocused = focusedId.empty() || agentId == focusedId;

  // Finalize this agent's streaming items using per-agent tracking
  auto* thinkItem = state_.agentThinkingItem(agentId);
  if (thinkItem && !thinkItem->isFinalized()) {
    thinkItem->finalize();
    state_.setAgentThinkingItem(agentId, nullptr);
    if (isFocused) state_.setActiveThinkingItem(nullptr);
  }
  markThinkingDone(state_, agentId);

  auto* textItem = state_.agentTextItem(agentId);
  if (textItem && !textItem->isFinalized()) {
    textItem->finalize();
    state_.setAgentTextItem(agentId, nullptr);
    if (isFocused) state_.setActiveTextItem(nullptr);
  }

  // Extract tool results from the turn's messages.
  // The turn data contains ToolResultContent entries with the actual results.
  // ALL tools (including process tools) are finalized here with the authoritative
  // result from the engine.
  rapidjson::Document doc;
  doc.Parse(json.c_str());
  if (!doc.HasParseError() && doc.HasMember("turn") && doc["turn"].IsObject()) {
    const auto& turn = doc["turn"];
    if (turn.HasMember("messages") && turn["messages"].IsArray()) {
      for (const auto& msg : turn["messages"].GetArray()) {
        if (!msg.HasMember("content") || !msg["content"].IsArray()) continue;
        for (const auto& part : msg["content"].GetArray()) {
          if (!part.HasMember("type") || !part["type"].IsString()) continue;
          if (std::string(part["type"].GetString()) != "toolResult") continue;

          std::string toolCallId = part.HasMember("toolCallId") ? part["toolCallId"].GetString() : "";
          std::string result = part.HasMember("result") ? part["result"].GetString() : "";
          bool success = part.HasMember("success") && part["success"].IsBool() && part["success"].GetBool();
          std::string processId = part.HasMember("processId") ? part["processId"].GetString() : "";

          if (toolCallId.empty()) continue;

          auto* tc = state_.findToolCallState(toolCallId);
          if (tc && tc->isFinished()) continue; // Already finished

          // Update processId mapping if we just learned it from the result
          if (!processId.empty() && tc && tc->processId.empty()) {
            tc->processId = processId;
            state_.mapProcessToTool(processId, toolCallId);
          }

          // Finalize ALL tools (including process tools) with the real result
          if (tc) {
            tc->phase = success ? ToolCallPhase::FinishedOk : ToolCallPhase::FinishedErr;
            tc->success = success;
            tc->result = result;
            tc->finishedAt = std::chrono::steady_clock::now();
          }

          auto* item = state_.findToolCallById(toolCallId);
          if (item) {
            item->setResult(success, result);
            // Keep Delegate tool calls live if their subagent is still running
            // so the live state display keeps updating
            bool keepLive = false;
            if (item->toolName() == "Delegate" || item->toolName() == "summon_subagent") {
              // Parse result to find child agentId
              rapidjson::Document resDoc;
              resDoc.Parse(result.c_str());
              if (!resDoc.HasParseError() && resDoc.HasMember("agentId") && resDoc["agentId"].IsString()) {
                std::string childId = resDoc["agentId"].GetString();
                const auto* childAgent = state_.findAgentState(childId);
                if (childAgent && childAgent->running) {
                  keepLive = true;
                }
              }
            }
            if (keepLive) {
              item->setLive(true);
            } else {
              item->setLive(false);
            }
          } else {
            // Quick-tool clusters don't create ToolCallItems. Update the
            // active cluster (if any) so it stops showing these entries
            // as inflight.
            auto* cluster = state_.agentQuickToolClusterItem(agentId);
            if (cluster && !cluster->isFinalized()) {
              cluster->setResult(toolCallId, success, result);
            }
          }
        }
      }
    }
  }

  if (!doc.HasParseError() && doc.HasMember("aggregateMetrics") &&
      doc["aggregateMetrics"].IsObject()) {
    const auto metrics =
        firmius::shared::agentMetricsFromJsonValue(doc["aggregateMetrics"]);
    auto* agent = state_.findAgentState(agentId);
    if (agent) {
      agent->contextUsedTokens = metrics.tokens.contextSize;
      agent->contextSentTokens = metrics.context.sentTokens;
    }
    if (agentId == state_.focusedAgentId()) {
      auto usage = state_.agentContextUsage();
      usage.usedTokens = metrics.tokens.contextSize;
      usage.sentTokens = metrics.context.sentTokens;
      state_.setAgentContextUsage(usage);
      const uint32_t window = std::max<uint32_t>(usage.windowTokens, 1);
      const uint32_t used =
          usage.usedTokens > 0 ? usage.usedTokens : usage.sentTokens;
      auto humanize = [](uint32_t value) {
        if (value >= 1000000) return std::to_string(value / 1000000) + "M";
        if (value >= 1000) return std::to_string(value / 1000) + "k";
        return std::to_string(value);
      };
      state_.setAgentContextWindow(humanize(used) + "/" + humanize(window));

      // Working-memory v2 telemetry latch.
      const auto prevMemory = state_.memoryStatus();
      const auto curMemory = memoryStatusFromMetrics(metrics.memory);
      state_.setMemoryStatus(curMemory);

      const std::string notice =
          formatMemoryActivityNotice(prevMemory, curMemory);
      if (!notice.empty()) {
        state_.addItem(std::make_unique<SystemNoticeItem>(notice));
      }
    }
  }

  // NOTE: No stale sweep here. handleAgentTurnCompleted processes all tools
  // that have results in this turn. Tools that don't have results yet are
  // legitimately still executing (e.g., process tools waiting for AgentProcessOutput).
  // handleAgentFinished is the last-resort finalizer when the agent exits.

  // Mark current turn as completed
  auto* agent = state_.findAgentState(agentId);
  if (agent && agent->currentTurn.has_value()) {
    agent->currentTurn->completed = true;
    agent->currentTurn.reset();
  }

  state_.markDirtyPublic();
}

void EventRouter::handleAgentFinished(const std::string &agentId) {
  const std::string focusedId = state_.focusedAgentId();
  bool isFocused = focusedId.empty() || agentId == focusedId;

  // Finalize this agent's streaming items using per-agent tracking
  auto* thinkItem = state_.agentThinkingItem(agentId);
  if (thinkItem && !thinkItem->isFinalized()) {
    thinkItem->finalize();
    state_.setAgentThinkingItem(agentId, nullptr);
    if (isFocused) state_.setActiveThinkingItem(nullptr);
  }

  auto* textItem = state_.agentTextItem(agentId);
  if (textItem && !textItem->isFinalized()) {
    textItem->finalize();
    state_.setAgentTextItem(agentId, nullptr);
    if (isFocused) state_.setActiveTextItem(nullptr);
  }

  // Last-resort: force-finish THIS agent's tool calls still stuck inflight.
  for (auto& [id, tc] : state_.toolCallsMut()) {
    if (!tc.isFinished() && tc.agentId == agentId) {
      std::string result = tc.result;
      bool success = tc.success;
      if (result.empty()) {
        auto* item = state_.findToolCallById(id);
        if (item && !item->processStdout().empty()) {
          result = "{\"stdout\":" + escapeJson(item->processStdout()) +
                   ",\"stderr\":" + escapeJson(item->processStderr()) +
                   ",\"exit_code\":" + std::to_string(item->processExitCode()) +
                   ",\"duration_ms\":" + std::to_string(static_cast<int>(item->processDurationMs())) +
                   ",\"process_id\":" + escapeJson(item->processId()) + "}";
          success = item->processExitCode() == 0;
        } else {
          result = "{\"error\":\"Tool result not received\"}";
          success = false;
        }
      }
      tc.phase = success ? ToolCallPhase::FinishedOk : ToolCallPhase::FinishedErr;
      tc.success = success;
      tc.result = result;
      tc.finishedAt = std::chrono::steady_clock::now();

      auto* item = state_.findToolCallById(id);
      if (item && item->phase() != ToolPhase::FinishedSuccess &&
          item->phase() != ToolPhase::FinishedError) {
        item->setResult(success, result);
        item->setLive(false);
      }
    }
  }

  // Mark agent turn as completed and not running
  auto* agent = state_.findAgentState(agentId);
  if (agent) {
    agent->currentTurn.reset();
    agent->running = false;
    agent->booting = false;
  }
  finalizeDelegateWaitsForAgent(state_, agentId, true, "Subagent completed");

  // If the finished agent was focused, auto-focus its parent (or primary)
  if (isFocused) {
    std::string parentId = agent ? agent->parentId : std::string();
    if (!parentId.empty()) {
      state_.focusAgent(parentId);
    } else {
      std::string primary = state_.primaryAgentId();
      if (!primary.empty() && primary != agentId) {
        state_.focusAgent(primary);
      }
    }
  }

  state_.markDirtyPublic();
}

void EventRouter::handleAgentSpawned(const std::string &json,
                                      const std::string &agentId) {
  rapidjson::Document doc;
  doc.Parse(json.c_str());
  if (doc.HasParseError()) return;

  std::string parentId = jsonString(doc, "parentId");
  std::string personaName = jsonString(doc, "personaName");
  std::string friendlyName = jsonString(doc, "friendlyName");
  std::string title = jsonString(doc, "title");
  std::string modelId = jsonString(doc, "modelId");
  std::string providerId = jsonString(doc, "providerId");

  // Deduplicate: if this is a root agent (no parent) and we already have a root agent
  // with the same persona AND same title (or both have no title), update the existing entry.
  // This handles the daemon sending agent_spawned multiple times (on reconnect, etc.)
  // Do NOT dedup agents with different titles — they are different agents.
  if (parentId.empty()) {
    for (auto& [existingId, existingAgent] : state_.agentsMut()) {
      if (existingAgent.parentId.empty() && existingId != agentId &&
          existingAgent.personaName == personaName &&
          (title.empty() || existingAgent.title.empty() || existingAgent.title == title)) {
        // Update the existing root agent with the new agentId
        existingAgent.agentId = agentId;
        existingAgent.running = true;
        existingAgent.booting = true;
        if (!title.empty()) existingAgent.title = title;
        if (!friendlyName.empty()) existingAgent.friendlyName = friendlyName;
        // Re-key the entry
        state_.renameAgent(existingId, agentId);
        // Update focus if needed
        if (state_.focusedAgentId() == existingId || state_.primaryAgentId() == existingId) {
          state_.focusAgent(agentId);
        }
        if (state_.primaryAgentId() == existingId) {
          state_.setPrimaryAgentId(agentId);
        }
        return;
      }
    }
  }

  // Create AgentState with all fields
  auto& agent = state_.getOrCreateAgent(agentId);
  agent.agentId = agentId;
  agent.parentId = parentId;
  agent.personaName = personaName;
  agent.friendlyName = friendlyName;
  agent.title = title;
  agent.providerId = providerId;
  agent.modelId = modelId;
  if (doc.HasMember("contextWindowTokens") && doc["contextWindowTokens"].IsUint()) {
    agent.contextWindowTokens = doc["contextWindowTokens"].GetUint();
  }
  if (doc.HasMember("contextUsedTokens") && doc["contextUsedTokens"].IsUint()) {
    agent.contextUsedTokens = doc["contextUsedTokens"].GetUint();
  }
  if (doc.HasMember("contextSentTokens") && doc["contextSentTokens"].IsUint()) {
    agent.contextSentTokens = doc["contextSentTokens"].GetUint();
  }
  agent.running = true;
  agent.booting = true;
  if (!parentId.empty()) {
    bindSpawnedChildToDelegate(state_, parentId, agentId);
  }

  // Start a new turn
  agent.currentTurn = AgentTurnState{};
  agent.currentTurn->agentId = agentId;

  // Focus logic:
  // - If this is a root agent (no parent) and no primary set, set as primary and focus
  // - Otherwise, don't change focus (user must manually switch with Ctrl+N/B/P)
  if (parentId.empty() && state_.primaryAgentId().empty()) {
    state_.setPrimaryAgentId(agentId);
    state_.setAgentId(agentId);
    state_.setAgentPurpose(personaName);
    if (!modelId.empty()) {
      std::string label = providerId.empty() ? modelId : providerId + "/" + modelId;
      state_.setModelLabel(label);
    }
    state_.setAgentContextUsage(ContextUsage{
        agent.contextWindowTokens,
        agent.contextUsedTokens,
        agent.contextSentTokens,
    });
    state_.focusAgent(agentId);
  }
}

void EventRouter::handleAgentError(const std::string &json,
                                    const std::string &agentId) {
  rapidjson::Document doc;
  doc.Parse(json.c_str());
  if (doc.HasParseError()) return;

  const std::string focusedId = state_.focusedAgentId();
  bool isFocused = focusedId.empty() || agentId == focusedId;

  // Finalize this agent's streaming items using per-agent tracking
  auto* thinkItem = state_.agentThinkingItem(agentId);
  if (thinkItem && !thinkItem->isFinalized()) {
    thinkItem->finalize();
    state_.setAgentThinkingItem(agentId, nullptr);
    if (isFocused) state_.setActiveThinkingItem(nullptr);
  }

  auto* textItem = state_.agentTextItem(agentId);
  if (textItem && !textItem->isFinalized()) {
    textItem->finalize();
    state_.setAgentTextItem(agentId, nullptr);
    if (isFocused) state_.setActiveTextItem(nullptr);
  }

  auto item = std::make_unique<ErrorMessageItem>(
      "\xe2\x9a\xa0 Error: " + jsonString(doc, "message"));
  state_.addItem(std::move(item));
  if (auto* cluster = state_.agentQuickToolClusterItem(agentId)) {
    if (!cluster->isFinalized()) cluster->finalize();
    state_.setAgentQuickToolClusterItem(agentId, nullptr);
  }
  finalizeDelegateWaitsForAgent(state_, agentId, false, jsonString(doc, "message"));
  state_.markDirtyPublic();
}

void EventRouter::handleAgentInterrupted(const std::string &agentId) {
  const std::string focusedId = state_.focusedAgentId();
  bool isFocused = focusedId.empty() || agentId == focusedId;

  // Finalize this agent's streaming items using per-agent tracking
  auto* thinkItem = state_.agentThinkingItem(agentId);
  if (thinkItem && !thinkItem->isFinalized()) {
    thinkItem->finalize();
    state_.setAgentThinkingItem(agentId, nullptr);
    if (isFocused) state_.setActiveThinkingItem(nullptr);
  }
  markThinkingDone(state_, agentId);

  auto* textItem = state_.agentTextItem(agentId);
  if (textItem && !textItem->isFinalized()) {
    textItem->finalize();
    state_.setAgentTextItem(agentId, nullptr);
    if (isFocused) state_.setActiveTextItem(nullptr);
  }

  // Force-finish THIS agent's inflight tool calls
  for (auto& [id, tc] : state_.toolCallsMut()) {
    if (!tc.isFinished() && tc.agentId == agentId) {
      tc.phase = ToolCallPhase::FinishedErr;
      tc.success = false;
      tc.finishedAt = std::chrono::steady_clock::now();

      auto* item = state_.findToolCallById(id);
      if (item) {
        item->setResult(false, "{}");
        item->setLive(false);
      }
    }
  }

  auto* agent = state_.findAgentState(agentId);
  if (agent) agent->currentTurn.reset();
  if (auto* cluster = state_.agentQuickToolClusterItem(agentId)) {
    if (!cluster->isFinalized()) cluster->finalize();
    state_.setAgentQuickToolClusterItem(agentId, nullptr);
  }
  finalizeDelegateWaitsForAgent(state_, agentId, false, "Subagent interrupted");

  state_.markDirtyPublic();
}

// ── User Messages ──

void EventRouter::handleUserMessageSent(const std::string &json) {
  rapidjson::Document doc;
  doc.Parse(json.c_str());
  if (doc.HasParseError()) return;

  std::string messageId = jsonString(doc, "messageId");
  std::string text = jsonString(doc, "text");
  std::string agentId = jsonString(doc, "agentId");
  if (text.empty()) return;

  if (auto* existing = state_.findUserMessageById(messageId)) {
    existing->setQueued(false);
    state_.markDirtyPublic();
    return;
  }

  auto item = std::make_unique<UserMessageItem>(std::move(text), std::move(agentId),
                                                std::move(messageId), false);
  state_.addItem(std::move(item));
  state_.markDirtyPublic();
}

void EventRouter::handleMessageQueued(const std::string &json) {
  rapidjson::Document doc;
  doc.Parse(json.c_str());
  if (doc.HasParseError()) {
    state_.queueMessageId("");
    return;
  }
  const std::string messageId = jsonString(doc, "messageId");
  const std::string text = jsonString(doc, "text");
  const std::string agentId = jsonString(doc, "agentId");

  state_.queueMessageId(messageId);
  state_.upsertQueuedUserMessage({messageId, text, agentId});

  if (auto* existing = state_.findUserMessageById(messageId)) {
    existing->setQueued(true);
  } else if (!text.empty()) {
    state_.addItem(
        std::make_unique<UserMessageItem>(text, agentId, messageId, true));
  }
  state_.markDirtyPublic();
}

void EventRouter::handleMessageDequeued(const std::string &json) {
  rapidjson::Document doc;
  doc.Parse(json.c_str());
  if (doc.HasParseError()) {
    state_.dequeueMessageId("");
    return;
  }
  const std::string messageId = jsonString(doc, "messageId");
  state_.dequeueMessageId(messageId);
  state_.removeQueuedUserMessage(messageId);
  if (auto* existing = state_.findUserMessageById(messageId)) {
    existing->setQueued(false);
  }
  state_.markDirtyPublic();
}

// ── Permissions ──

void EventRouter::handlePermissionEscalation(const std::string &json) {
  rapidjson::Document doc;
  doc.Parse(json.c_str());
  if (doc.HasParseError()) return;

  std::string toolCallId = jsonString(doc, "toolCallId");

  auto perm = pendingPermissionFromJson(doc);
  std::string reqId = perm.requestId;  // Save before move
  state_.pushPendingPermission(std::move(perm));

  // Link permission to tool call — keep at Preparing while waiting
  if (!toolCallId.empty()) {
    auto* tc = state_.findToolCallState(toolCallId);
    if (tc) {
      tc->pendingPermissionId = reqId;
    }
    auto* item = state_.findToolCallById(toolCallId);
    if (item) {
      // Stay at Preparing — user hasn't approved yet
      item->setLive(true);
    }
  }

  state_.markDirtyPublic();
}

void EventRouter::handlePermissionResolved(const std::string &json) {
  rapidjson::Document doc;
  doc.Parse(json.c_str());
  std::string toolCallId;
  std::string requestId;
  if (!doc.HasParseError()) {
    if (doc.HasMember("toolCallId") && doc["toolCallId"].IsString())
      toolCallId = doc["toolCallId"].GetString();
    if (doc.HasMember("requestId") && doc["requestId"].IsString())
      requestId = doc["requestId"].GetString();
  }

  // Pop only this specific permission from the queue
  if (!requestId.empty()) {
    state_.popPendingPermission(requestId);
  }

  // Advance the tool call from Preparing to Called now that permission is granted
  if (!toolCallId.empty()) {
    auto* tc = state_.findToolCallState(toolCallId);
    if (tc) {
      tc->pendingPermissionId.clear();
      if (tc->phase == ToolCallPhase::Streaming || tc->phase == ToolCallPhase::Prepared) {
        tc->phase = ToolCallPhase::Prepared;
      }
    }
    auto* item = state_.findToolCallById(toolCallId);
    if (item && item->phase() == ToolPhase::Preparing) {
      item->setPhase(ToolPhase::Called);
    }
  }
}

void EventRouter::handleAgentTodoUpdated(const std::string &json) {
  rapidjson::Document doc;
  doc.Parse(json.c_str());
  if (doc.HasParseError()) return;

  std::string agentId = jsonString(doc, "agentId");
  if (agentId.empty()) return;

  std::vector<firmius::shared::TodoItem> items;
  if (doc.HasMember("todo") && doc["todo"].IsObject() &&
      doc["todo"].HasMember("items") && doc["todo"]["items"].IsArray()) {
    for (const auto &item : doc["todo"]["items"].GetArray()) {
      items.push_back(firmius::shared::todoItemFromJson(item));
    }
  }
  state_.setAgentTodos(agentId, items);
}

// ── Misc ──

void EventRouter::handleModelSwitched(const std::string &json) {
  rapidjson::Document doc;
  doc.Parse(json.c_str());
  if (doc.HasParseError()) return;

  std::string modelId = jsonString(doc, "newModelId");
  std::string providerId = jsonString(doc, "newProviderId");
  std::string agentId = jsonString(doc, "agentId");
  if (!modelId.empty()) {
    std::string label = modelId;
    if (!providerId.empty()) {
      label = providerId + "/" + modelId;
    }
    state_.setModelLabel(label);
  }
  if (!agentId.empty()) {
    state_.updateAgentModel(agentId, providerId, modelId);
  }
}

void EventRouter::handleThreadTitleUpdated(const std::string &json) {
  rapidjson::Document doc;
  doc.Parse(json.c_str());
  if (doc.HasParseError()) return;

  std::string title = jsonString(doc, "title");
  if (!title.empty()) {
    state_.setThreadTitle(title);
  }
}

void EventRouter::handleThreadMetadataUpdated(const std::string &json) {
  rapidjson::Document doc;
  doc.Parse(json.c_str());
  if (doc.HasParseError()) return;
  // Pick up incidental metadata changes (title, etc.). Active mode no
  // longer rides on ThreadMetadata — modes live in the policy engine
  // and are pulled in via getPermissionMode RPCs after thread load.
  if (doc.HasMember("metadata") && doc["metadata"].IsObject()) {
    const auto &m = doc["metadata"];
    if (m.HasMember("title") && m["title"].IsString()) {
      state_.setThreadTitle(m["title"].GetString());
    }
  }
  state_.markDirtyPublic();
}

void EventRouter::handleConfigUpdated() {
  state_.markDirtyPublic();
}

void EventRouter::handleHookStateChanged(
    const firmius::daemon::HookStateSnapshot &snapshot) {
  state_.setHookState(snapshot);
}

void EventRouter::flushPendingProcessOutput(const std::string &processId) {
  auto it = pendingProcessOutput_.find(processId);
  if (it == pendingProcessOutput_.end()) return;

  std::string toolCallId = state_.findToolCallByProcessId(processId);
  if (toolCallId.empty()) return;

  auto* toolItem = state_.findToolCallById(toolCallId);
  auto* tc = state_.findToolCallState(toolCallId);
  if (!toolItem) {
    pendingProcessOutput_.erase(it);
    return;
  }

  for (const auto &pending : it->second) {
    if (!pending.output.empty()) {
      toolItem->appendProcessOutput(pending.output, pending.isStderr);
      if (tc) {
        if (pending.isStderr) tc->processStderr += pending.output;
        else tc->processStdout += pending.output;
      }
    }
    if (pending.finished) {
      if (tc) {
        tc->processExitCode = pending.exitCode;
        tc->processDurationMs = pending.durationMs;
        tc->processFinished = true;
      }
      toolItem->setProcessExitInfo(pending.exitCode, pending.durationMs);
    }
  }

  pendingProcessOutput_.erase(it);
}

void EventRouter::handleEmbeddingModelProgress(const std::string &json) {
  rapidjson::Document doc;
  doc.Parse(json.c_str());
  if (doc.HasParseError()) return;

  state_.setEmbeddingDownload(embeddingDownloadStateFromProgress(doc));
}

void EventRouter::handleAgentCompacting(const std::string & /*agentId*/) {
}

void EventRouter::handleContextCompacted(const std::string & /*json*/,
                                          const std::string & /*agentId*/) {
}

} // namespace firmius::tui

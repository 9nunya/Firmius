#include "EventRouter.hpp"
#include "items/SimpleItems.hpp"
#include "items/StreamingItems.hpp"
#include "items/ToolCallItem.hpp"

#include "utils/ToolView.hpp"

#include <cstdio>
#include <rapidjson/document.h>

namespace firmius::tui2 {

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
    break;
  case firmius::daemon::DaemonEventKind::PactStateChanged:
    break;
  case firmius::daemon::DaemonEventKind::InitProgress:
    if (!envelope.initMessage.empty()) {
      state_.addItem(std::make_unique<SystemNoticeItem>(envelope.initMessage));
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
    handleMessageQueued();
  } else if (eventType == "message_dequeued") {
    handleMessageDequeued();
  } else if (eventType == "permission_escalation_request") {
    handlePermissionEscalation(eventJson);
  } else if (eventType == "permission_escalation_resolved") {
    handlePermissionResolved(eventJson);
  } else if (eventType == "model_switched") {
    handleModelSwitched(eventJson);
  } else if (eventType == "thread_title_updated") {
    handleThreadTitleUpdated(eventJson);
  } else if (eventType == "config_updated") {
    handleConfigUpdated();
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

} // namespace

// ── Streaming text/thinking (unchanged) ──

void EventRouter::handleAgentText(const std::string &json,
                                   const std::string &agentId) {
  rapidjson::Document doc;
  doc.Parse(json.c_str());
  if (doc.HasParseError()) return;

  std::string delta = jsonString(doc, "delta");
  if (delta.empty()) return;

  bool isFocused = (agentId == state_.focusedAgentId());

  if (isFocused) {
    auto* thinkItem = state_.activeThinkingItem();
    if (thinkItem && !thinkItem->isFinalized()) {
      thinkItem->finalize();
      state_.setActiveThinkingItem(nullptr);
    }
  }

  auto* item = state_.activeTextItem();
  if (!item || item->agentId() != agentId) {
    // Need a new text item for this agent
    auto newItem = std::make_unique<AgentTextItem>();
    newItem->setAgentId(agentId);
    item = newItem.get();
    state_.addItem(std::move(newItem));
    if (isFocused) {
      state_.setActiveTextItem(item);
    }
  }
  item->appendDelta(delta);
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

  bool isFocused = (agentId == state_.focusedAgentId());

  auto* item = state_.activeThinkingItem();
  if (!item || item->agentId() != agentId) {
    if (isFocused) {
      auto* textItem = state_.activeTextItem();
      if (textItem && !textItem->isFinalized()) {
        textItem->finalize();
      }
    }

    auto newItem = std::make_unique<AgentThinkingItem>();
    newItem->setAgentId(agentId);
    item = newItem.get();
    state_.addItem(std::move(newItem));
    if (isFocused) {
      state_.setActiveThinkingItem(item);
    }
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

  // Finalize active streaming items
  auto* textItem = state_.activeTextItem();
  if (textItem && !textItem->isFinalized()) {
    textItem->finalize();
    state_.setActiveTextItem(nullptr);
  }
  auto* thinkItem = state_.activeThinkingItem();
  if (thinkItem && !thinkItem->isFinalized()) {
    thinkItem->finalize();
    state_.setActiveThinkingItem(nullptr);
  }

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

  // Process tools stay at Preparing until AgentProcessSpawned arrives
  // (which means permission was resolved and the process actually started).
  // Non-process tools (blocking) go directly to Called.
  bool isProcessTool = (toolName == "Process" || toolName == "Python" ||
                        toolName == "Delegate");

  // Find or create ToolCallItem in transcript
  auto* existingItem = state_.findToolCallById(toolCallId);
  if (existingItem) {
    existingItem->setArgs(toolArgs);
    if (!isProcessTool) {
      existingItem->setPhase(ToolPhase::Called);
    }
    // else: stay at Preparing — AgentProcessSpawned will advance to Called
    existingItem->setLive(true);
  } else {
    auto item = std::make_unique<ToolCallItem>(toolCallId, toolName, agentId);
    item->setAppState(&state_);
    item->setArgs(toolArgs);
    if (!isProcessTool) {
      item->setPhase(ToolPhase::Called);
    }
    // else: default phase is Preparing, which is correct
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
                                             const std::string & /*agentId*/) {
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
  bool isFocused = (agentId == state_.focusedAgentId());

  // Finalize active streaming items if they belong to this agent
  auto* thinkItem = state_.activeThinkingItem();
  if (thinkItem && !thinkItem->isFinalized() && thinkItem->agentId() == agentId) {
    thinkItem->finalize();
    if (isFocused) state_.setActiveThinkingItem(nullptr);
  }

  auto* textItem = state_.activeTextItem();
  if (textItem && !textItem->isFinalized() && textItem->agentId() == agentId) {
    textItem->finalize();
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
            item->setLive(false);
          }
        }
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
  bool isFocused = (agentId == state_.focusedAgentId());

  // Finalize streaming items only if they belong to this agent
  auto* thinkItem = state_.activeThinkingItem();
  if (thinkItem && !thinkItem->isFinalized() && thinkItem->agentId() == agentId) {
    thinkItem->finalize();
    if (isFocused) state_.setActiveThinkingItem(nullptr);
  }

  auto* textItem = state_.activeTextItem();
  if (textItem && !textItem->isFinalized() && textItem->agentId() == agentId) {
    textItem->finalize();
    if (isFocused) state_.setActiveTextItem(nullptr);
  }

  // Last-resort: force-finish any tool calls still stuck inflight.
  for (auto& [id, tc] : state_.toolCallsMut()) {
    if (!tc.isFinished()) {
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

  // Create AgentState with all fields
  auto& agent = state_.getOrCreateAgent(agentId);
  agent.agentId = agentId;
  agent.parentId = parentId;
  agent.personaName = personaName;
  agent.friendlyName = friendlyName;
  agent.title = title;
  agent.providerId = providerId;
  agent.modelId = modelId;
  agent.running = true;
  agent.booting = true;

  // Start a new turn
  agent.currentTurn = AgentTurnState{};
  agent.currentTurn->agentId = agentId;

  // Auto-focus logic:
  // - If this is a root agent (no parent) and no primary set, set as primary and focus
  // - If this is a child of the currently focused agent, auto-focus it
  // - Otherwise, don't change focus
  if (parentId.empty() && state_.primaryAgentId().empty()) {
    state_.setPrimaryAgentId(agentId);
    state_.setAgentId(agentId);
    state_.setAgentPurpose(personaName);
    if (!modelId.empty()) {
      std::string label = providerId.empty() ? modelId : providerId + "/" + modelId;
      state_.setModelLabel(label);
    }
    if (doc.HasMember("maxTokens") && doc["maxTokens"].IsNumber()) {
      state_.setAgentContextWindow(std::to_string(doc["maxTokens"].GetInt() / 1000) + "k");
    }
    state_.focusAgent(agentId);
  } else if (!parentId.empty() && parentId == state_.focusedAgentId()) {
    // Auto-focus child of focused agent
    state_.focusAgent(agentId);
  }
}

void EventRouter::handleAgentError(const std::string &json,
                                    const std::string &agentId) {
  rapidjson::Document doc;
  doc.Parse(json.c_str());
  if (doc.HasParseError()) return;

  bool isFocused = (agentId == state_.focusedAgentId());

  // Finalize streaming items if they belong to this agent
  auto* thinkItem = state_.activeThinkingItem();
  if (thinkItem && !thinkItem->isFinalized() && thinkItem->agentId() == agentId) {
    thinkItem->finalize();
    if (isFocused) state_.setActiveThinkingItem(nullptr);
  }

  auto* textItem = state_.activeTextItem();
  if (textItem && !textItem->isFinalized() && textItem->agentId() == agentId) {
    textItem->finalize();
    if (isFocused) state_.setActiveTextItem(nullptr);
  }

  auto item = std::make_unique<ErrorMessageItem>(
      "\xe2\x9a\xa0 Error: " + jsonString(doc, "message"));
  state_.addItem(std::move(item));
  state_.markDirtyPublic();
}

void EventRouter::handleAgentInterrupted(const std::string &agentId) {
  bool isFocused = (agentId == state_.focusedAgentId());

  // Finalize streaming items if they belong to this agent
  auto* thinkItem = state_.activeThinkingItem();
  if (thinkItem && !thinkItem->isFinalized() && thinkItem->agentId() == agentId) {
    thinkItem->finalize();
    if (isFocused) state_.setActiveThinkingItem(nullptr);
  }

  auto* textItem = state_.activeTextItem();
  if (textItem && !textItem->isFinalized() && textItem->agentId() == agentId) {
    textItem->finalize();
    if (isFocused) state_.setActiveTextItem(nullptr);
  }

  // Force-finish inflight tool calls
  for (auto& [id, tc] : state_.toolCallsMut()) {
    if (!tc.isFinished()) {
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

  state_.markDirtyPublic();
}

// ── User Messages ──

void EventRouter::handleUserMessageSent(const std::string &json) {
  rapidjson::Document doc;
  doc.Parse(json.c_str());
  if (doc.HasParseError()) return;

  std::string text = jsonString(doc, "text");
  if (text.empty()) return;

  const auto& items = state_.items();
  if (!items.empty()) {
    auto* last = items.back().get();
    if (last->type() == "UserMessage") {
      auto* um = static_cast<UserMessageItem*>(last);
      if (um->text() == text) return;
    }
  }

  auto item = std::make_unique<UserMessageItem>(std::move(text));
  state_.addItem(std::move(item));
  state_.markDirtyPublic();
}

void EventRouter::handleMessageQueued() {
  state_.setQueuedMessageCount(state_.queuedMessageCount() + 1);
}

void EventRouter::handleMessageDequeued() {
  int count = state_.queuedMessageCount();
  state_.setQueuedMessageCount(count > 0 ? count - 1 : 0);
}

// ── Permissions ──

void EventRouter::handlePermissionEscalation(const std::string &json) {
  rapidjson::Document doc;
  doc.Parse(json.c_str());
  if (doc.HasParseError()) return;

  std::string toolCallId = jsonString(doc, "toolCallId");

  PendingPermission perm;
  perm.requestId = jsonString(doc, "requestId");
  perm.title = jsonString(doc, "title");
  perm.message = jsonString(doc, "message");
  perm.toolName = jsonString(doc, "toolName");
  if (doc.HasMember("allowAlways") && doc["allowAlways"].IsBool()) {
    perm.allowAlways = doc["allowAlways"].GetBool();
  }
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

// ── Misc ──

void EventRouter::handleModelSwitched(const std::string &json) {
  rapidjson::Document doc;
  doc.Parse(json.c_str());
  if (doc.HasParseError()) return;

  std::string modelId = jsonString(doc, "newModelId");
  std::string providerId = jsonString(doc, "newProviderId");
  if (!modelId.empty()) {
    std::string label = modelId;
    if (!providerId.empty()) {
      label = providerId + "/" + modelId;
    }
    state_.setModelLabel(label);
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

void EventRouter::handleConfigUpdated() {
  state_.markDirtyPublic();
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

} // namespace firmius::tui2

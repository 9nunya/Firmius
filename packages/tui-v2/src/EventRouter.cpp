#include "EventRouter.hpp"

#include <rapidjson/document.h>

namespace firmius::tui2 {

EventRouter::EventRouter(AppState &state) : state_(state) {}

void EventRouter::route(const firmius::daemon::DaemonEventEnvelope &envelope) {
  switch (envelope.kind) {
  case firmius::daemon::DaemonEventKind::RuntimeAppEvent:
    routeRuntimeEvent(envelope.runtimeEventType,
                      envelope.runtimeEventJson,
                      envelope.runtimeEventThreadId,
                      envelope.runtimeEventAgentId);
    break;
  case firmius::daemon::DaemonEventKind::ClientSessionRegistered:
  case firmius::daemon::DaemonEventKind::ClientSessionDisconnected:
  case firmius::daemon::DaemonEventKind::ClientSessionUpdated:
    // Session events — no action needed in tui-v2 for now.
    break;
  case firmius::daemon::DaemonEventKind::HookStateChanged:
    // Future: update hook status in state.
    break;
  case firmius::daemon::DaemonEventKind::PactStateChanged:
    // Future: update pact status in state.
    break;
  }
}

void EventRouter::routeRuntimeEvent(const std::string &eventType,
                                     const std::string &eventJson,
                                     const std::string & /*threadId*/,
                                     const std::string &agentId) {
  if (eventType == "agent_text") {
    handleAgentText(eventJson, agentId);
  } else if (eventType == "agent_thinking") {
    handleAgentThinking(eventJson, agentId);
  } else if (eventType == "agent_tool_call") {
    handleAgentToolCall(eventJson, agentId);
  } else if (eventType == "agent_turn_completed") {
    handleAgentTurnCompleted(agentId);
  } else if (eventType == "agent_finished") {
    handleAgentFinished(agentId);
  } else if (eventType == "agent_spawned") {
    handleAgentSpawned(eventJson, agentId);
  } else if (eventType == "agent_error") {
    handleAgentError(eventJson, agentId);
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
  } else if (eventType == "agent_process_output") {
    handleAgentProcessOutput(eventJson, agentId);
  }
  // Unknown event types are silently ignored — forward compatible.
}

namespace {

std::string jsonString(const rapidjson::Document &doc, const char *field) {
  if (doc.HasMember(field) && doc[field].IsString()) {
    return doc[field].GetString();
  }
  return "";
}

} // namespace

void EventRouter::handleAgentText(const std::string &json,
                                   const std::string & /*agentId*/) {
  rapidjson::Document doc;
  doc.Parse(json.c_str());
  if (doc.HasParseError()) return;
  state_.appendStreamingDelta(jsonString(doc, "delta"));
}

void EventRouter::handleAgentThinking(const std::string &json,
                                       const std::string &agentId) {
  rapidjson::Document doc;
  doc.Parse(json.c_str());
  if (doc.HasParseError()) return;

  TranscriptLine line;
  line.kind = TranscriptLine::Kind::Thinking;
  line.text = jsonString(doc, "delta");
  line.agentId = agentId;
  state_.appendTranscriptLine(std::move(line));
}

void EventRouter::handleAgentToolCall(const std::string &json,
                                       const std::string &agentId) {
  rapidjson::Document doc;
  doc.Parse(json.c_str());
  if (doc.HasParseError()) return;

  std::string toolCallId = jsonString(doc, "toolCallId");
  std::string toolName = jsonString(doc, "toolName");

  // Add to active tool calls.
  ActiveToolCall call;
  call.toolCallId = toolCallId;
  call.toolName = toolName;
  call.agentId = agentId;
  call.status = "running";
  state_.addActiveToolCall(std::move(call));

  // Add to transcript.
  TranscriptLine line;
  line.kind = TranscriptLine::Kind::ToolCall;
  line.toolCallId = toolCallId;
  line.toolName = toolName;
  line.agentId = agentId;
  line.text = "⚙ " + toolName;
  state_.appendTranscriptLine(std::move(line));
}

void EventRouter::handleAgentTurnCompleted(const std::string & /*agentId*/) {
  state_.finalizeStreamingLine();
}

void EventRouter::handleAgentFinished(const std::string & /*agentId*/) {
  state_.finalizeStreamingLine();
  state_.setAgentStatus(firmius::shared::AgentStatus::Idle);
}

void EventRouter::handleAgentSpawned(const std::string &json,
                                      const std::string &agentId) {
  rapidjson::Document doc;
  doc.Parse(json.c_str());
  if (doc.HasParseError()) return;

  state_.setAgentId(agentId);
  state_.setAgentStatus(firmius::shared::AgentStatus::Streaming);
  state_.setAgentPurpose(jsonString(doc, "personaName"));
  
  if (doc.HasMember("maxTokens") && doc["maxTokens"].IsNumber()) {
    state_.setAgentContextWindow(std::to_string(doc["maxTokens"].GetInt() / 1000) + "k");
  }

  std::string modelId = jsonString(doc, "modelId");
  std::string providerId = jsonString(doc, "providerId");
  if (!modelId.empty()) {
    std::string label = modelId;
    if (!providerId.empty()) {
      label = providerId + "/" + modelId;
    }
    state_.setModelLabel(label);
  }
}

void EventRouter::handleAgentError(const std::string &json,
                                    const std::string &agentId) {
  rapidjson::Document doc;
  doc.Parse(json.c_str());
  if (doc.HasParseError()) return;

  TranscriptLine line;
  line.kind = TranscriptLine::Kind::Notice;
  line.text = "⚠ Error: " + jsonString(doc, "message");
  line.agentId = agentId;
  state_.appendTranscriptLine(std::move(line));
}

void EventRouter::handleUserMessageSent(const std::string &json) {
  rapidjson::Document doc;
  doc.Parse(json.c_str());
  if (doc.HasParseError()) return;

  TranscriptLine line;
  line.kind = TranscriptLine::Kind::UserMessage;
  line.text = jsonString(doc, "text");
  state_.appendTranscriptLine(std::move(line));
}

void EventRouter::handleMessageQueued() {
  state_.setQueuedMessageCount(state_.queuedMessageCount() + 1);
}

void EventRouter::handleMessageDequeued() {
  int count = state_.queuedMessageCount();
  state_.setQueuedMessageCount(count > 0 ? count - 1 : 0);
}

void EventRouter::handlePermissionEscalation(const std::string &json) {
  rapidjson::Document doc;
  doc.Parse(json.c_str());
  if (doc.HasParseError()) return;

  PendingPermission perm;
  perm.requestId = jsonString(doc, "requestId");
  perm.title = jsonString(doc, "title");
  perm.message = jsonString(doc, "message");
  perm.toolName = jsonString(doc, "toolName");
  if (doc.HasMember("allowAlways") && doc["allowAlways"].IsBool()) {
    perm.allowAlways = doc["allowAlways"].GetBool();
  }
  state_.setPendingPermission(std::move(perm));
}

void EventRouter::handlePermissionResolved(const std::string & /*json*/) {
  state_.clearPendingPermission();
}

void EventRouter::handleAgentProcessOutput(const std::string &json,
                                            const std::string &agentId) {
  rapidjson::Document doc;
  doc.Parse(json.c_str());
  if (doc.HasParseError()) return;

  std::string output = jsonString(doc, "output");
  if (!output.empty()) {
    TranscriptLine line;
    line.kind = TranscriptLine::Kind::System;
    line.text = output;
    line.agentId = agentId;
    state_.appendTranscriptLine(std::move(line));
  }
}

} // namespace firmius::tui2

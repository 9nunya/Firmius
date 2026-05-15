#include "EventRouter.hpp"

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
  } else if (eventType == "model_switched") {
    handleModelSwitched(eventJson);
  } else if (eventType == "config_updated") {
    handleConfigUpdated();
  }

  if (realStatus.has_value()) {
    state_.setAgentStatus(*realStatus);
  }
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
                                       const std::string & /*agentId*/) {
  rapidjson::Document doc;
  doc.Parse(json.c_str());
  if (doc.HasParseError()) return;
  state_.appendStreamingThinkingDelta(jsonString(doc, "delta"));
}

void EventRouter::handleAgentToolCall(const std::string &json,
                                       const std::string &agentId) {
  rapidjson::Document doc;
  doc.Parse(json.c_str());
  if (doc.HasParseError()) return;

  std::string toolCallId = jsonString(doc, "toolCallId");
  std::string toolName = jsonString(doc, "toolName");

  ActiveToolCall call;
  call.toolCallId = toolCallId;
  call.toolName = toolName;
  call.agentId = agentId;
  call.status = "running";
  state_.addActiveToolCall(std::move(call));

  TranscriptLine line;
  line.kind = TranscriptLine::Kind::ToolCall;
  line.toolCallId = toolCallId;
  line.toolName = toolName;
  line.agentId = agentId;
  line.text = "⚙ " + toolName;
  state_.appendTranscriptLine(std::move(line));
}

void EventRouter::handleAgentTurnCompleted(const std::string & /*agentId*/) {
  state_.finalizeStreamingThinkingLine();
  state_.finalizeStreamingLine();
}

void EventRouter::handleAgentFinished(const std::string & /*agentId*/) {
  state_.finalizeStreamingThinkingLine();
  state_.finalizeStreamingLine();
}

void EventRouter::handleAgentSpawned(const std::string &json,
                                      const std::string &agentId) {
  rapidjson::Document doc;
  doc.Parse(json.c_str());
  if (doc.HasParseError()) return;

  state_.setAgentId(agentId);
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

  state_.finalizeStreamingThinkingLine();
  state_.finalizeStreamingLine();

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

  std::string text = jsonString(doc, "text");
  auto lines = state_.transcriptLines();
  if (!lines.empty() && lines.back().kind == TranscriptLine::Kind::UserMessage &&
      lines.back().text == text) {
    return;
  }

  TranscriptLine line;
  line.kind = TranscriptLine::Kind::UserMessage;
  line.text = std::move(text);
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

void EventRouter::handleConfigUpdated() {
  state_.markDirtyPublic();
}

} // namespace firmius::tui2

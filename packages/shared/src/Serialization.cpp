#include "Serialization.hpp"

#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>
#include <stdexcept>

namespace firmius::shared {

namespace {

// Enum conversion helpers
std::string roleToString(Role value) {
  switch (value) {
  case Role::System:
    return "System";
  case Role::User:
    return "User";
  case Role::Assistant:
    return "Assistant";
  case Role::ToolResult:
    return "ToolResult";
  case Role::Error:
    return "Error";
  }
  return "Unknown";
}

Role stringToRole(const std::string &str) {
  if (str == "System")
    return Role::System;
  if (str == "User")
    return Role::User;
  if (str == "Assistant")
    return Role::Assistant;
  if (str == "ToolResult")
    return Role::ToolResult;
  if (str == "Error")
    return Role::Error;
  throw std::runtime_error("Unknown Role: " + str);
}

std::string hostTypeToString(HostType value) {
  switch (value) {
  case HostType::Local:
    return "Local";
  case HostType::Docker:
    return "Docker";
  case HostType::RemoteSSH:
    return "RemoteSSH";
  }
  return "Unknown";
}

HostType stringToHostType(const std::string &str) {
  if (str == "Local")
    return HostType::Local;
  if (str == "Docker")
    return HostType::Docker;
  if (str == "RemoteSSH")
    return HostType::RemoteSSH;
  throw std::runtime_error("Unknown HostType: " + str);
}

std::string toolScopeToString(ToolScope value) {
  switch (value) {
  case ToolScope::FilesystemRead:
    return "FilesystemRead";
  case ToolScope::FilesystemWrite:
    return "FilesystemWrite";
  case ToolScope::Process:
    return "Process";
  case ToolScope::Semantic:
    return "Semantic";
  case ToolScope::Delegation:
    return "Delegation";
  case ToolScope::Web:
    return "Web";
  case ToolScope::Git:
    return "Git";
  }
  return "Unknown";
}

ToolScope stringToToolScope(const std::string &str) {
  if (str == "FilesystemRead")
    return ToolScope::FilesystemRead;
  if (str == "FilesystemWrite")
    return ToolScope::FilesystemWrite;
  if (str == "Process")
    return ToolScope::Process;
  if (str == "Semantic")
    return ToolScope::Semantic;
  if (str == "Delegation")
    return ToolScope::Delegation;
  if (str == "Web")
    return ToolScope::Web;
  if (str == "Git")
    return ToolScope::Git;
  throw std::runtime_error("Unknown ToolScope: " + str);
}

std::string agentStatusToString(AgentStatus value) {
  switch (value) {
  case AgentStatus::Idle:
    return "Idle";
  case AgentStatus::Streaming:
    return "Streaming";
  case AgentStatus::ExecutingTool:
    return "ExecutingTool";
  case AgentStatus::AwaitingInput:
    return "AwaitingInput";
  case AgentStatus::Compacting:
    return "Compacting";
  case AgentStatus::ProviderWaiting:
    return "ProviderWaiting";
  case AgentStatus::Error:
    return "Error";
  case AgentStatus::Cancelled:
    return "Cancelled";
  }
  return "Unknown";
}

AgentStatus stringToAgentStatus(const std::string &str) {
  if (str == "Idle")
    return AgentStatus::Idle;
  if (str == "Streaming")
    return AgentStatus::Streaming;
  if (str == "ExecutingTool")
    return AgentStatus::ExecutingTool;
  if (str == "AwaitingInput")
    return AgentStatus::AwaitingInput;
  if (str == "Compacting")
    return AgentStatus::Compacting;
  if (str == "Error")
    return AgentStatus::Error;
  if (str == "Cancelled")
    return AgentStatus::Cancelled;
  throw std::runtime_error("Unknown AgentStatus: " + str);
}

std::string stopReasonToString(StopReason value) {
  switch (value) {
  case StopReason::Stop:
    return "Stop";
  case StopReason::ToolUse:
    return "ToolUse";
  case StopReason::MaxTokens:
    return "MaxTokens";
  case StopReason::ContentFilter:
    return "ContentFilter";
  case StopReason::Error:
    return "Error";
  case StopReason::Cancelled:
    return "Cancelled";
  }
  return "Unknown";
}

StopReason stringToStopReason(const std::string &str) {
  if (str == "Stop")
    return StopReason::Stop;
  if (str == "ToolUse")
    return StopReason::ToolUse;
  if (str == "MaxTokens")
    return StopReason::MaxTokens;
  if (str == "ContentFilter")
    return StopReason::ContentFilter;
  if (str == "Error")
    return StopReason::Error;
  if (str == "Cancelled")
    return StopReason::Cancelled;
  throw std::runtime_error("Unknown StopReason: " + str);
}

// Struct conversion helpers
rapidjson::Value tokenMetricsToJson(const TokenMetrics &m,
                                    rapidjson::Document::AllocatorType &a) {
  rapidjson::Value v(rapidjson::kObjectType);
  v.AddMember("prompt", m.prompt, a);
  v.AddMember("completion", m.completion, a);
  v.AddMember("reasoning", m.reasoning, a);
  v.AddMember("cacheRead", m.cacheRead, a);
  v.AddMember("cacheWrite", m.cacheWrite, a);
  v.AddMember("contextSize", m.contextSize, a);
  v.AddMember("cumulativePrompt", m.cumulativePrompt, a);
  v.AddMember("total", m.total, a);
  return v;
}

TokenMetrics tokenMetricsFromJson(const rapidjson::Value &v) {
  TokenMetrics tm;
  tm.prompt =
      v.HasMember("prompt") && v["prompt"].IsUint() ? v["prompt"].GetUint() : 0;
  tm.completion = v.HasMember("completion") && v["completion"].IsUint()
                      ? v["completion"].GetUint()
                      : 0;
  tm.reasoning = v.HasMember("reasoning") && v["reasoning"].IsUint()
                     ? v["reasoning"].GetUint()
                     : 0;
  tm.cacheRead = v.HasMember("cacheRead") && v["cacheRead"].IsUint()
                     ? v["cacheRead"].GetUint()
                     : 0;
  tm.cacheWrite = v.HasMember("cacheWrite") && v["cacheWrite"].IsUint()
                      ? v["cacheWrite"].GetUint()
                      : 0;
  tm.contextSize = v.HasMember("contextSize") && v["contextSize"].IsUint()
                       ? v["contextSize"].GetUint()
                       : 0;
  tm.cumulativePrompt =
      v.HasMember("cumulativePrompt") && v["cumulativePrompt"].IsUint()
          ? v["cumulativePrompt"].GetUint()
          : 0;
  tm.total =
      v.HasMember("total") && v["total"].IsUint() ? v["total"].GetUint() : 0;
  return tm;
}

rapidjson::Value timingMetricsToJson(const TimingMetrics &m,
                                     rapidjson::Document::AllocatorType &a) {
  rapidjson::Value v(rapidjson::kObjectType);
  v.AddMember("startMs", m.startMs, a);
  v.AddMember("firstTokenMs", m.firstTokenMs, a);
  v.AddMember("endMs", m.endMs, a);
  v.AddMember("toolExecutionMs", m.toolExecutionMs, a);
  return v;
}

TimingMetrics timingMetricsFromJson(const rapidjson::Value &v) {
  return {v["startMs"].GetUint64(), v["firstTokenMs"].GetUint64(),
          v["endMs"].GetUint64(), v["toolExecutionMs"].GetUint64()};
}

rapidjson::Value agentMetricsToJson(const AgentMetrics &m,
                                    rapidjson::Document::AllocatorType &a) {
  rapidjson::Value v(rapidjson::kObjectType);
  v.AddMember("tokens", tokenMetricsToJson(m.tokens, a), a);
  v.AddMember("timing", timingMetricsToJson(m.timing, a), a);
  v.AddMember("estimatedCostUsd", m.estimatedCostUsd, a);
  return v;
}

AgentMetrics agentMetricsFromJson(const rapidjson::Value &v) {
  return {tokenMetricsFromJson(v["tokens"]), timingMetricsFromJson(v["timing"]),
          v["estimatedCostUsd"].GetDouble()};
}

rapidjson::Value
hostCreationOptionsToJson(const HostCreationOptions &o,
                          rapidjson::Document::AllocatorType &a) {
  rapidjson::Value v(rapidjson::kObjectType);
  v.AddMember("type", rapidjson::Value(hostTypeToString(o.type).c_str(), a), a);
  v.AddMember("containerName", rapidjson::Value(o.containerName.c_str(), a), a);
  v.AddMember("connectToExisting", o.connectToExisting, a);
  v.AddMember("deleteOnExit", o.deleteOnExit, a);
  return v;
}

HostCreationOptions hostCreationOptionsFromJson(const rapidjson::Value &v) {
  HostCreationOptions o;
  if (v.HasMember("type")) {
    o.type = stringToHostType(v["type"].GetString());
  }
  if (v.HasMember("containerName")) {
    o.containerName = v["containerName"].GetString();
  }
  if (v.HasMember("connectToExisting")) {
    o.connectToExisting = v["connectToExisting"].GetBool();
  }
  if (v.HasMember("deleteOnExit")) {
    o.deleteOnExit = v["deleteOnExit"].GetBool();
  }
  return o;
}

rapidjson::Value messagePartToJson(const MessagePart &p,
                                   rapidjson::Document::AllocatorType &a) {
  rapidjson::Value v(rapidjson::kObjectType);
  if (auto *txt = std::get_if<TextContent>(&p)) {
    v.AddMember("type", "text", a);
    v.AddMember("text", rapidjson::Value(txt->text.c_str(), a), a);
  } else if (auto *thk = std::get_if<ThinkingContent>(&p)) {
    v.AddMember("type", "thinking", a);
    v.AddMember("thinking", rapidjson::Value(thk->thinking.c_str(), a), a);
  } else if (auto *tcc = std::get_if<ToolCallContent>(&p)) {
    v.AddMember("type", "toolCall", a);
    v.AddMember("id", rapidjson::Value(tcc->id.c_str(), a), a);
    v.AddMember("name", rapidjson::Value(tcc->name.c_str(), a), a);
    v.AddMember("args", rapidjson::Value(tcc->args.c_str(), a), a);
  } else if (auto *trc = std::get_if<ToolResultContent>(&p)) {
    v.AddMember("type", "toolResult", a);
    v.AddMember("toolCallId", rapidjson::Value(trc->toolCallId.c_str(), a), a);
    v.AddMember("result", rapidjson::Value(trc->result.c_str(), a), a);
    v.AddMember("success", trc->success, a);
    v.AddMember("processId", rapidjson::Value(trc->processId.c_str(), a), a);
    v.AddMember("subagentId", rapidjson::Value(trc->subagentId.c_str(), a), a);
  } else if (auto *img = std::get_if<ImageContent>(&p)) {
    v.AddMember("type", "image", a);
    v.AddMember("url", rapidjson::Value(img->url.c_str(), a), a);
    v.AddMember("mediaType", rapidjson::Value(img->mediaType.c_str(), a), a);
    v.AddMember("detail", rapidjson::Value(img->detail.c_str(), a), a);
  } else if (auto *err = std::get_if<ErrorContent>(&p)) {
    v.AddMember("type", "error", a);
    v.AddMember("errorName", rapidjson::Value(err->errorName.c_str(), a), a);
    v.AddMember("description", rapidjson::Value(err->description.c_str(), a),
                a);
    v.AddMember("details", rapidjson::Value(err->details.c_str(), a), a);
  }
  return v;
}

MessagePart messagePartFromJson(const rapidjson::Value &v) {
  std::string type = v["type"].GetString();
  if (type == "text")
    return TextContent{v["text"].GetString()};
  if (type == "thinking")
    return ThinkingContent{v["thinking"].GetString(), ""};
  if (type == "toolCall")
    return ToolCallContent{v["id"].GetString(), v["name"].GetString(),
                           v["args"].GetString()};
  if (type == "toolResult")
    return ToolResultContent{
        v["toolCallId"].GetString(), v["result"].GetString(),
        v["success"].GetBool(),
        v.HasMember("processId") ? v["processId"].GetString() : "",
        v.HasMember("subagentId") ? v["subagentId"].GetString() : ""};
  if (type == "image")
    return ImageContent{v["url"].GetString(), v["mediaType"].GetString(),
                        v["detail"].GetString()};
  if (type == "error")
    return ErrorContent{v["errorName"].GetString(),
                        v["description"].GetString(), v["details"].GetString()};
  throw std::runtime_error("Unknown MessagePart type: " + type);
}

rapidjson::Value messageToJson(const Message &m,
                               rapidjson::Document::AllocatorType &a) {
  rapidjson::Value v(rapidjson::kObjectType);
  v.AddMember("id", rapidjson::Value(m.id.c_str(), a), a);
  v.AddMember("role", rapidjson::Value(roleToString(m.role).c_str(), a), a);
  rapidjson::Value content(rapidjson::kArrayType);
  for (const auto &p : m.content)
    content.PushBack(messagePartToJson(p, a), a);
  v.AddMember("content", content, a);
  v.AddMember("timestamp", m.timestamp, a);
  if (m.parentId)
    v.AddMember("parentId", rapidjson::Value(m.parentId->c_str(), a), a);
  else
    v.AddMember("parentId", rapidjson::Value(rapidjson::kNullType), a);
  return v;
}

Message messageFromJson(const rapidjson::Value &v) {
  Message m;
  m.id = v["id"].GetString();
  m.role = stringToRole(v["role"].GetString());
  for (const auto &p : v["content"].GetArray())
    m.content.push_back(messagePartFromJson(p));
  m.timestamp = v["timestamp"].GetUint64();
  if (v["parentId"].IsString())
    m.parentId = v["parentId"].GetString();
  return m;
}

} // namespace

rapidjson::Document toJson(const HostCreationOptions &o) {
  rapidjson::Document d;
  auto &a = d.GetAllocator();
  d.CopyFrom(hostCreationOptionsToJson(o, a), a);
  return d;
}

HostCreationOptions
hostCreationOptionsFromJsonValue(const rapidjson::Value &v) {
  return hostCreationOptionsFromJson(v);
}

rapidjson::Document toJson(const AgentContext &ctx) {
  rapidjson::Document d;
  d.SetObject();
  auto &a = d.GetAllocator();

  rapidjson::Value identity(rapidjson::kObjectType);
  identity.AddMember("id", rapidjson::Value(ctx.identity.id.c_str(), a), a);
  identity.AddMember("name", rapidjson::Value(ctx.identity.name.c_str(), a), a);
  identity.AddMember("role", rapidjson::Value(ctx.identity.role.c_str(), a), a);
  identity.AddMember("goal", rapidjson::Value(ctx.identity.goal.c_str(), a), a);
  identity.AddMember("systemPrompt",
                     rapidjson::Value(ctx.identity.systemPrompt.c_str(), a), a);
  identity.AddMember("parentId",
                     rapidjson::Value(ctx.identity.parentId.c_str(), a), a);
  d.AddMember("identity", identity, a);

  rapidjson::Value permissions(rapidjson::kObjectType);
  rapidjson::Value scopes(rapidjson::kArrayType);
  for (auto s : ctx.permissions.allowedScopes)
    scopes.PushBack(rapidjson::Value(toolScopeToString(s).c_str(), a), a);
  permissions.AddMember("allowedScopes", scopes, a);
  rapidjson::Value paths(rapidjson::kArrayType);
  for (const auto &p : ctx.permissions.allowedPaths)
    paths.PushBack(rapidjson::Value(p.c_str(), a), a);
  permissions.AddMember("allowedPaths", paths, a);
  permissions.AddMember("allowOutsideCwd", ctx.permissions.allowOutsideCwd, a);
  d.AddMember("permissions", permissions, a);

  rapidjson::Value env(rapidjson::kObjectType);
  env.AddMember(
      "type",
      rapidjson::Value(hostTypeToString(ctx.environment.type).c_str(), a), a);
  env.AddMember("identifier",
                rapidjson::Value(ctx.environment.identifier.c_str(), a), a);
  env.AddMember("cwd", rapidjson::Value(ctx.environment.cwd.c_str(), a), a);
  rapidjson::Value envVars(rapidjson::kObjectType);
  for (const auto &[k, v] : ctx.environment.envVars)
    envVars.AddMember(rapidjson::Value(k.c_str(), a),
                      rapidjson::Value(v.c_str(), a), a);
  env.AddMember("envVars", envVars, a);
  d.AddMember("environment", env, a);

  const AgentHistory emptyHistory{};
  const AgentHistory *historyPtr =
      ctx.history ? ctx.history.get() : &emptyHistory;
  rapidjson::Value history(rapidjson::kObjectType);
  history.AddMember("threadId",
                    rapidjson::Value(historyPtr->threadId.c_str(), a), a);
  rapidjson::Value turns(rapidjson::kArrayType);
  for (const auto &t : historyPtr->turns) {
    rapidjson::Value turn(rapidjson::kObjectType);
    turn.AddMember("turnId", rapidjson::Value(t.turnId.c_str(), a), a);
    rapidjson::Value msgs(rapidjson::kArrayType);
    for (const auto &m : t.messages)
      msgs.PushBack(messageToJson(m, a), a);
    turn.AddMember("messages", msgs, a);
    turn.AddMember("metrics", agentMetricsToJson(t.metrics, a), a);
    turn.AddMember(
        "stopReason",
        rapidjson::Value(stopReasonToString(t.stopReason).c_str(), a), a);
    turns.PushBack(turn, a);
  }
  history.AddMember("turns", turns, a);
  d.AddMember("history", history, a);

  rapidjson::Value state(rapidjson::kObjectType);
  state.AddMember(
      "currentStatus",
      rapidjson::Value(agentStatusToString(ctx.state.currentStatus).c_str(), a),
      a);
  rapidjson::Value pending(rapidjson::kArrayType);
  for (const auto &p : ctx.state.pendingToolCalls)
    pending.PushBack(rapidjson::Value(p.c_str(), a), a);
  state.AddMember("pendingToolCalls", pending, a);
  rapidjson::Value procs(rapidjson::kArrayType);
  for (const auto &p : ctx.state.ownedProcesses)
    procs.PushBack(rapidjson::Value(p.c_str(), a), a);
  state.AddMember("ownedProcesses", procs, a);
  rapidjson::Value readFiles(rapidjson::kArrayType);
  for (const auto &f : ctx.state.readFiles)
    readFiles.PushBack(rapidjson::Value(f.c_str(), a), a);
  state.AddMember("readFiles", readFiles, a);
  
  rapidjson::Value fullyReadFiles(rapidjson::kArrayType);
  for (const auto &f : ctx.state.fullyReadFiles)
    fullyReadFiles.PushBack(rapidjson::Value(f.c_str(), a), a);
  state.AddMember("fullyReadFiles", fullyReadFiles, a);
  rapidjson::Value editedFilesArray(rapidjson::kArrayType);
  for (const auto &f : ctx.state.editedFiles)
    editedFilesArray.PushBack(rapidjson::Value(f.c_str(), a), a);
  state.AddMember("editedFiles", editedFilesArray, a);
  rapidjson::Value completedActionsArray(rapidjson::kArrayType);
  for (const auto &act : ctx.state.completedActions)
    completedActionsArray.PushBack(rapidjson::Value(act.c_str(), a), a);
  state.AddMember("completedActions", completedActionsArray, a);
  if (ctx.state.fatalError)
    state.AddMember("fatalError",
                    rapidjson::Value(ctx.state.fatalError->c_str(), a), a);
  else
    state.AddMember("fatalError", rapidjson::Value(rapidjson::kNullType), a);
  // Write blocking process IDs as an array
  rapidjson::Value blockingArray(rapidjson::kArrayType);
  for (const auto &pid : ctx.state.blockingProcessIds) {
    blockingArray.PushBack(rapidjson::Value(pid.c_str(), a), a);
  }
  state.AddMember("blockingProcessIds", blockingArray, a);
  d.AddMember("state", state, a);

  rapidjson::Value config(rapidjson::kObjectType);
  config.AddMember("modelId", rapidjson::Value(ctx.config.modelId.c_str(), a),
                   a);
  config.AddMember("modelVariant",
                   rapidjson::Value(ctx.config.modelVariant.c_str(), a), a);
  config.AddMember("personaName",
                   rapidjson::Value(ctx.config.personaName.c_str(), a), a);
  config.AddMember("maxTurns", ctx.config.maxTurns, a);
  config.AddMember("temperature", ctx.config.temperature, a);
  if (ctx.config.maxTokens) {
    config.AddMember("maxTokens", ctx.config.maxTokens.value(), a);
  } else {
    config.AddMember("maxTokens", rapidjson::Value(rapidjson::kNullType), a);
  }
  rapidjson::Value stopSeqs(rapidjson::kArrayType);
  for (const auto &s : ctx.config.stop)
    stopSeqs.PushBack(rapidjson::Value(s.c_str(), a), a);
  config.AddMember("stop", stopSeqs, a);
  config.AddMember("persistHistory", ctx.config.persistHistory, a);
  d.AddMember("config", config, a);

  d.AddMember("aggregateMetrics", agentMetricsToJson(ctx.aggregateMetrics, a),
              a);

  return d;
}

AgentContext fromJson(const rapidjson::Value &v) {
  AgentContext ctx;
  ctx.identity.id = v["identity"]["id"].GetString();
  ctx.identity.name = v["identity"]["name"].GetString();
  ctx.identity.role = v["identity"]["role"].GetString();
  ctx.identity.goal = v["identity"]["goal"].GetString();
  ctx.identity.systemPrompt = v["identity"]["systemPrompt"].GetString();
  ctx.identity.parentId = v["identity"].HasMember("parentId")
                              ? v["identity"]["parentId"].GetString()
                              : "";
  ctx.identity.friendlyName = v["identity"].HasMember("friendlyName")
                                  ? v["identity"]["friendlyName"].GetString()
                                  : "";
  for (const auto &s : v["permissions"]["allowedScopes"].GetArray())
    ctx.permissions.allowedScopes.push_back(stringToToolScope(s.GetString()));
  for (const auto &p : v["permissions"]["allowedPaths"].GetArray())
    ctx.permissions.allowedPaths.push_back(p.GetString());
  ctx.permissions.allowOutsideCwd =
      v["permissions"]["allowOutsideCwd"].GetBool();
  ctx.environment.type = stringToHostType(v["environment"]["type"].GetString());
  ctx.environment.identifier = v["environment"]["identifier"].GetString();
  ctx.environment.cwd = v["environment"]["cwd"].GetString();
  for (auto it = v["environment"]["envVars"].MemberBegin();
       it != v["environment"]["envVars"].MemberEnd(); ++it)
    ctx.environment.envVars[it->name.GetString()] = it->value.GetString();
  ctx.history = std::make_shared<AgentHistory>();
  ctx.history->threadId = v["history"]["threadId"].GetString();
  for (const auto &t : v["history"]["turns"].GetArray()) {
    AgentTurn turn;
    turn.turnId = t["turnId"].GetString();
    for (const auto &m : t["messages"].GetArray())
      turn.messages.push_back(messageFromJson(m));
    turn.metrics = agentMetricsFromJson(t["metrics"]);
    if (t.HasMember("stopReason") && t["stopReason"].IsString()) {
      turn.stopReason = stringToStopReason(t["stopReason"].GetString());
    }
    ctx.history->turns.push_back(turn);
  }
  ctx.state.currentStatus =
      stringToAgentStatus(v["state"]["currentStatus"].GetString());
  for (const auto &p : v["state"]["pendingToolCalls"].GetArray())
    ctx.state.pendingToolCalls.push_back(p.GetString());
  for (const auto &p : v["state"]["ownedProcesses"].GetArray())
    ctx.state.ownedProcesses.push_back(p.GetString());
  if (v["state"].HasMember("readFiles") && v["state"]["readFiles"].IsArray()) {
    for (const auto &f : v["state"]["readFiles"].GetArray())
      ctx.state.readFiles.push_back(f.GetString());
  }
  if (v["state"].HasMember("fullyReadFiles") && v["state"]["fullyReadFiles"].IsArray()) {
    for (const auto &f : v["state"]["fullyReadFiles"].GetArray())
      ctx.state.fullyReadFiles.push_back(f.GetString());
  }
  if (v["state"].HasMember("editedFiles") &&
      v["state"]["editedFiles"].IsArray()) {
    for (const auto &f : v["state"]["editedFiles"].GetArray())
      ctx.state.editedFiles.push_back(f.GetString());
  }
  if (v["state"].HasMember("completedActions") &&
      v["state"]["completedActions"].IsArray()) {
    for (const auto &act : v["state"]["completedActions"].GetArray())
      ctx.state.completedActions.push_back(act.GetString());
  }
  if (v["state"]["fatalError"].IsString())
    ctx.state.fatalError = v["state"]["fatalError"].GetString();
  // Load blockingProcessIds (new format) or currentBlockingProcessId (legacy)
  if (v["state"].HasMember("blockingProcessIds") &&
      v["state"]["blockingProcessIds"].IsArray()) {
    for (const auto &pid : v["state"]["blockingProcessIds"].GetArray()) {
      ctx.state.blockingProcessIds.push_back(pid.GetString());
    }
  } else if (v["state"].HasMember("currentBlockingProcessId") &&
             v["state"]["currentBlockingProcessId"].IsString()) {
    ctx.state.blockingProcessIds.push_back(
        v["state"]["currentBlockingProcessId"].GetString());
  }

  if (v.HasMember("config") && v["config"].IsObject()) {
    const auto &cfg = v["config"];
    if (cfg.HasMember("modelId"))
      ctx.config.modelId = cfg["modelId"].GetString();
    if (cfg.HasMember("modelVariant"))
      ctx.config.modelVariant = cfg["modelVariant"].GetString();
    if (cfg.HasMember("personaName"))
      ctx.config.personaName = cfg["personaName"].GetString();
    if (cfg.HasMember("maxTurns"))
      ctx.config.maxTurns = cfg["maxTurns"].GetInt();
    if (cfg.HasMember("temperature"))
      ctx.config.temperature = cfg["temperature"].GetFloat();
    if (cfg.HasMember("maxTokens") && cfg["maxTokens"].IsUint())
      ctx.config.maxTokens = cfg["maxTokens"].GetUint();
    if (cfg.HasMember("stop") && cfg["stop"].IsArray()) {
      for (const auto &s : cfg["stop"].GetArray())
        ctx.config.stop.push_back(s.GetString());
    }
    if (cfg.HasMember("persistHistory"))
      ctx.config.persistHistory = cfg["persistHistory"].GetBool();
  }

  ctx.aggregateMetrics = agentMetricsFromJson(v["aggregateMetrics"]);
  return ctx;
}

rapidjson::Document toJson(const Message &msg) {
  rapidjson::Document d;
  auto &a = d.GetAllocator();
  d.CopyFrom(messageToJson(msg, a), a);
  return d;
}

Message messageFromJsonValue(const rapidjson::Value &v) {
  return messageFromJson(v);
}

rapidjson::Document toJson(const AgentTurn &turn) {
  rapidjson::Document d;
  d.SetObject();
  auto &a = d.GetAllocator();
  d.AddMember("turnId", rapidjson::Value(turn.turnId.c_str(), a), a);
  rapidjson::Value msgs(rapidjson::kArrayType);
  for (const auto &m : turn.messages)
    msgs.PushBack(messageToJson(m, a), a);
  d.AddMember("messages", msgs, a);
  d.AddMember("metrics", agentMetricsToJson(turn.metrics, a), a);
  d.AddMember("stopReason",
              rapidjson::Value(stopReasonToString(turn.stopReason).c_str(), a),
              a);
  return d;
}

AgentTurn agentTurnFromJsonValue(const rapidjson::Value &v) {
  AgentTurn t;
  t.turnId = v["turnId"].GetString();
  for (const auto &m : v["messages"].GetArray())
    t.messages.push_back(messageFromJson(m));
  t.metrics = agentMetricsFromJson(v["metrics"]);
  if (v.HasMember("stopReason") && v["stopReason"].IsString()) {
    t.stopReason = stringToStopReason(v["stopReason"].GetString());
  }
  return t;
}

rapidjson::Document toJson(const StreamEvent &ev) {
  rapidjson::Document d;
  d.SetObject();
  auto &a = d.GetAllocator();
  if (auto *txt = std::get_if<TextChunk>(&ev)) {
    d.AddMember("type", "text", a);
    d.AddMember("delta", rapidjson::Value(txt->delta.c_str(), a), a);
  } else if (auto *thk = std::get_if<ThinkingChunk>(&ev)) {
    d.AddMember("type", "thinking", a);
    d.AddMember("delta", rapidjson::Value(thk->delta.c_str(), a), a);
  } else if (auto *tcc = std::get_if<ToolCallChunk>(&ev)) {
    d.AddMember("type", "toolCall", a);
    d.AddMember("id", rapidjson::Value(tcc->id.c_str(), a), a);
    d.AddMember("index", tcc->index, a);
    d.AddMember("nameDelta", rapidjson::Value(tcc->nameDelta.c_str(), a), a);
    d.AddMember("argsDelta", rapidjson::Value(tcc->argsDelta.c_str(), a), a);
  } else if (auto *met = std::get_if<AgentMetrics>(&ev)) {
    d.AddMember("type", "metrics", a);
    rapidjson::Value m = agentMetricsToJson(*met, a);
    for (auto it = m.MemberBegin(); it != m.MemberEnd(); ++it)
      d.AddMember(rapidjson::Value(it->name, a), rapidjson::Value(it->value, a),
                  a);
  } else if (auto *done = std::get_if<StreamDone>(&ev)) {
    d.AddMember("type", "done", a);
    d.AddMember("reason",
                rapidjson::Value(stopReasonToString(done->reason).c_str(), a),
                a);
  } else if (auto *err = std::get_if<StreamError>(&ev)) {
    d.AddMember("type", "error", a);
    d.AddMember("message", rapidjson::Value(err->message.c_str(), a), a);
    d.AddMember("httpStatus", err->httpStatus, a);
  } else if (auto *tc = std::get_if<AgentTurnCompleted>(&ev)) {
    d.AddMember("type", "turnCompleted", a);
    d.AddMember("agentId", rapidjson::Value(tc->agentId.c_str(), a), a);
    d.AddMember("turn", toJson(tc->turn).Move(), a);
    d.AddMember("aggregateMetrics", toJson(tc->aggregateMetrics).Move(), a);
  } else if (auto *ac = std::get_if<AgentCompacting>(&ev)) {
    d.AddMember("type", "compacting", a);
    d.AddMember("agentId", rapidjson::Value(ac->agentId.c_str(), a), a);
  } else if (auto *act = std::get_if<AgentCompactionThinking>(&ev)) {
    d.AddMember("type", "compactionThinking", a);
    d.AddMember("agentId", rapidjson::Value(act->agentId.c_str(), a), a);
    d.AddMember("delta", rapidjson::Value(act->delta.c_str(), a), a);
  } else if (auto *acx = std::get_if<AgentCompactionText>(&ev)) {
    d.AddMember("type", "compactionText", a);
    d.AddMember("agentId", rapidjson::Value(acx->agentId.c_str(), a), a);
    d.AddMember("delta", rapidjson::Value(acx->delta.c_str(), a), a);
  } else if (auto *cc = std::get_if<ContextCompacted>(&ev)) {
    d.AddMember("type", "compacted", a);
    d.AddMember("agentId", rapidjson::Value(cc->agentId.c_str(), a), a);
    d.AddMember("tokensSaved", cc->tokensSaved, a);
  } else if (auto *pod = std::get_if<ProcessOutputDelta>(&ev)) {
    d.AddMember("type", "processOutput", a);
    d.AddMember("processId", rapidjson::Value(pod->processId.c_str(), a), a);
    d.AddMember("output", rapidjson::Value(pod->output.c_str(), a), a);
    d.AddMember("isStderr", pod->isStderr, a);
    d.AddMember("finished", pod->finished, a);
  }
  return d;
}

StreamEvent streamEventFromJsonValue(const rapidjson::Value &v) {
  std::string type = v["type"].GetString();
  if (type == "text")
    return TextChunk{v["delta"].GetString()};
  if (type == "thinking")
    return ThinkingChunk{v["delta"].GetString(), ""};
  if (type == "toolCall")
    return ToolCallChunk{v["id"].GetString(), v["index"].GetUint(),
                         v["nameDelta"].GetString(),
                         v["argsDelta"].GetString()};
  if (type == "metrics")
    return agentMetricsFromJson(v);
  if (type == "done")
    return StreamDone{stringToStopReason(v["reason"].GetString())};
  if (type == "error")
    return StreamError{v["message"].GetString(),
                       v.HasMember("httpStatus") ? v["httpStatus"].GetInt() : 0,
                       ""};
  if (type == "turnCompleted")
    return AgentTurnCompleted{
        v["agentId"].GetString(), agentTurnFromJsonValue(v["turn"]),
        agentMetricsFromJsonValue(v["aggregateMetrics"]),
        v.HasMember("parentId") ? v["parentId"].GetString() : ""};
  if (type == "compacting")
    return AgentCompacting{v["agentId"].GetString(),
                           v.HasMember("parentId") ? v["parentId"].GetString()
                                                   : ""};
  if (type == "compactionThinking")
    return AgentCompactionThinking{
        v["agentId"].GetString(), v["delta"].GetString(),
        v.HasMember("parentId") ? v["parentId"].GetString() : ""};
  if (type == "compactionText")
    return AgentCompactionText{
        v["agentId"].GetString(), v["delta"].GetString(),
        v.HasMember("parentId") ? v["parentId"].GetString() : ""};
  if (type == "compacted")
    return ContextCompacted{
        v["agentId"].GetString(), v["tokensSaved"].GetUint(),
        v.HasMember("parentId") ? v["parentId"].GetString() : ""};
  if (type == "processOutput")
    return ProcessOutputDelta{v["processId"].GetString(),
                              v["output"].GetString(), v["isStderr"].GetBool(),
                              v["finished"].GetBool()};
  throw std::runtime_error("Unknown StreamEvent type: " + type);
}

rapidjson::Document toJson(const MessagePart &part) {
  rapidjson::Document d;
  auto &a = d.GetAllocator();
  d.CopyFrom(messagePartToJson(part, a), a);
  return d;
}

MessagePart messagePartFromJsonValue(const rapidjson::Value &v) {
  return messagePartFromJson(v);
}

rapidjson::Document toJson(const AgentMetrics &metrics) {
  rapidjson::Document d;
  auto &a = d.GetAllocator();
  d.CopyFrom(agentMetricsToJson(metrics, a), a);
  return d;
}

AgentMetrics agentMetricsFromJsonValue(const rapidjson::Value &v) {
  return agentMetricsFromJson(v);
}

rapidjson::Document toJson(const ThreadMetadata &m) {
  rapidjson::Document d;
  d.SetObject();
  auto &a = d.GetAllocator();
  d.AddMember("threadId", rapidjson::Value(m.threadId.c_str(), a), a);
  d.AddMember("title", rapidjson::Value(m.title.c_str(), a), a);
  d.AddMember("hostOptions", hostCreationOptionsToJson(m.hostOptions, a), a);
  d.AddMember("hostIdentifier", rapidjson::Value(m.hostIdentifier.c_str(), a),
              a);
  d.AddMember("cwd", rapidjson::Value(m.cwd.c_str(), a), a);
  d.AddMember("leadPersona", rapidjson::Value(m.leadPersona.c_str(), a), a);
  d.AddMember("createdAt", m.createdAt, a);
  d.AddMember("lastActiveAt", m.lastActiveAt, a);
  return d;
}

ThreadMetadata threadMetadataFromJson(const rapidjson::Value &v) {
  ThreadMetadata m;
  m.threadId = v.HasMember("threadId") && v["threadId"].IsString()
                   ? v["threadId"].GetString()
                   : "";
  m.title = v.HasMember("title") && v["title"].IsString()
                ? v["title"].GetString()
                : "Untitled Thread";
  if (v.HasMember("hostOptions") && v["hostOptions"].IsObject()) {
    m.hostOptions = hostCreationOptionsFromJson(v["hostOptions"]);
  } else if (v.HasMember("hostType") && v["hostType"].IsString()) {
    m.hostOptions.type = stringToHostType(v["hostType"].GetString());
  }
  m.hostIdentifier =
      v.HasMember("hostIdentifier") && v["hostIdentifier"].IsString()
          ? v["hostIdentifier"].GetString()
          : "";
  m.cwd = v.HasMember("cwd") && v["cwd"].IsString() ? v["cwd"].GetString() : "";
  m.leadPersona = v.HasMember("leadPersona") && v["leadPersona"].IsString()
                      ? v["leadPersona"].GetString()
                      : "";
  m.createdAt = v.HasMember("createdAt") && v["createdAt"].IsUint64()
                    ? v["createdAt"].GetUint64()
                    : 0;
  m.lastActiveAt = v.HasMember("lastActiveAt") && v["lastActiveAt"].IsUint64()
                       ? v["lastActiveAt"].GetUint64()
                       : 0;
  return m;
}

rapidjson::Document toJson(const EngineEvent &ev) {
  rapidjson::Document d;
  d.SetObject();
  auto &a = d.GetAllocator();

  if (auto *s = std::get_if<AgentSpawned>(&ev)) {
    d.AddMember("type", "AgentSpawned", a);
    d.AddMember("agentId", rapidjson::Value(s->agentId.c_str(), a), a);
    d.AddMember("personaName", rapidjson::Value(s->personaName.c_str(), a), a);
    d.AddMember("parentId", rapidjson::Value(s->parentId.c_str(), a), a);
  } else if (auto *t = std::get_if<AgentThinking>(&ev)) {
    d.AddMember("type", "AgentThinking", a);
    d.AddMember("agentId", rapidjson::Value(t->agentId.c_str(), a), a);
    d.AddMember("delta", rapidjson::Value(t->delta.c_str(), a), a);
    d.AddMember("parentId", rapidjson::Value(t->parentId.c_str(), a), a);
  } else if (auto *tx = std::get_if<AgentText>(&ev)) {
    d.AddMember("type", "AgentText", a);
    d.AddMember("agentId", rapidjson::Value(tx->agentId.c_str(), a), a);
    d.AddMember("delta", rapidjson::Value(tx->delta.c_str(), a), a);
    d.AddMember("parentId", rapidjson::Value(tx->parentId.c_str(), a), a);
  } else if (auto *tc = std::get_if<AgentToolCall>(&ev)) {
    d.AddMember("type", "AgentToolCall", a);
    d.AddMember("agentId", rapidjson::Value(tc->agentId.c_str(), a), a);
    d.AddMember("toolCallId", rapidjson::Value(tc->toolCallId.c_str(), a), a);
    d.AddMember("toolName", rapidjson::Value(tc->toolName.c_str(), a), a);
    d.AddMember("toolArgs", rapidjson::Value(tc->toolArgs.c_str(), a), a);
    d.AddMember("parentId", rapidjson::Value(tc->parentId.c_str(), a), a);
  } else if (auto *tc = std::get_if<AgentTurnCompleted>(&ev)) {
    d.AddMember("type", "AgentTurnCompleted", a);
    d.AddMember("agentId", rapidjson::Value(tc->agentId.c_str(), a), a);
    d.AddMember("turn", toJson(tc->turn).Move(), a);
    d.AddMember("aggregateMetrics", toJson(tc->aggregateMetrics).Move(), a);
    d.AddMember("parentId", rapidjson::Value(tc->parentId.c_str(), a), a);
  } else if (auto *c = std::get_if<AgentCompleted>(&ev)) {
    d.AddMember("type", "AgentCompleted", a);
    d.AddMember("agentId", rapidjson::Value(c->agentId.c_str(), a), a);
    d.AddMember("summary", rapidjson::Value(c->summary.c_str(), a), a);
    d.AddMember("parentId", rapidjson::Value(c->parentId.c_str(), a), a);
  } else if (auto *e = std::get_if<AgentError>(&ev)) {
    d.AddMember("type", "AgentError", a);
    d.AddMember("agentId", rapidjson::Value(e->agentId.c_str(), a), a);
    d.AddMember("message", rapidjson::Value(e->message.c_str(), a), a);
    d.AddMember("parentId", rapidjson::Value(e->parentId.c_str(), a), a);
  } else if (auto *ac = std::get_if<AgentCompacting>(&ev)) {
    d.AddMember("type", "AgentCompacting", a);
    d.AddMember("agentId", rapidjson::Value(ac->agentId.c_str(), a), a);
    d.AddMember("parentId", rapidjson::Value(ac->parentId.c_str(), a), a);
  } else if (auto *act = std::get_if<AgentCompactionThinking>(&ev)) {
    d.AddMember("type", "AgentCompactionThinking", a);
    d.AddMember("agentId", rapidjson::Value(act->agentId.c_str(), a), a);
    d.AddMember("delta", rapidjson::Value(act->delta.c_str(), a), a);
    d.AddMember("parentId", rapidjson::Value(act->parentId.c_str(), a), a);
  } else if (auto *acx = std::get_if<AgentCompactionText>(&ev)) {
    d.AddMember("type", "AgentCompactionText", a);
    d.AddMember("agentId", rapidjson::Value(acx->agentId.c_str(), a), a);
    d.AddMember("delta", rapidjson::Value(acx->delta.c_str(), a), a);
    d.AddMember("parentId", rapidjson::Value(acx->parentId.c_str(), a), a);
  } else if (auto *cc = std::get_if<ContextCompacted>(&ev)) {
    d.AddMember("type", "ContextCompacted", a);
    d.AddMember("agentId", rapidjson::Value(cc->agentId.c_str(), a), a);
    d.AddMember("tokensSaved", cc->tokensSaved, a);
    d.AddMember("parentId", rapidjson::Value(cc->parentId.c_str(), a), a);
  } else if (auto *apo = std::get_if<AgentProcessOutput>(&ev)) {
    d.AddMember("type", "AgentProcessOutput", a);
    d.AddMember("agentId", rapidjson::Value(apo->agentId.c_str(), a), a);
    d.AddMember("processId", rapidjson::Value(apo->processId.c_str(), a), a);
    d.AddMember("output", rapidjson::Value(apo->output.c_str(), a), a);
    d.AddMember("isStderr", apo->isStderr, a);
    d.AddMember("finished", apo->finished, a);
    d.AddMember("parentId", rapidjson::Value(apo->parentId.c_str(), a), a);
  }

  return d;
}

EngineEvent engineEventFromJson(const rapidjson::Value &v) {
  std::string type = v["type"].GetString();
  if (type == "AgentSpawned")
    return AgentSpawned{
        v["agentId"].GetString(),
        v["personaName"].GetString(),
        v.HasMember("parentId") ? v["parentId"].GetString() : "",
        v.HasMember("friendlyName") ? v["friendlyName"].GetString() : "",
        v.HasMember("title") ? v["title"].GetString() : "",
        v.HasMember("persistHistory") ? v["persistHistory"].GetBool() : false};
  if (type == "AgentThinking")
    return AgentThinking{v["agentId"].GetString(), v["delta"].GetString(),
                         v.HasMember("parentId") ? v["parentId"].GetString()
                                                 : ""};
  if (type == "AgentText")
    return AgentText{v["agentId"].GetString(), v["delta"].GetString(),
                     v.HasMember("parentId") ? v["parentId"].GetString() : ""};
  if (type == "AgentToolCall")
    return AgentToolCall{
        v["agentId"].GetString(),
        v.HasMember("toolCallId") ? v["toolCallId"].GetString() : "",
        v["toolName"].GetString(), v["toolArgs"].GetString(),
        v.HasMember("parentId") ? v["parentId"].GetString() : ""};
  if (type == "AgentTurnCompleted")
    return AgentTurnCompleted{
        v["agentId"].GetString(), agentTurnFromJsonValue(v["turn"]),
        agentMetricsFromJsonValue(v["aggregateMetrics"]),
        v.HasMember("parentId") ? v["parentId"].GetString() : ""};
  if (type == "AgentCompleted")
    return AgentCompleted{v["agentId"].GetString(), v["summary"].GetString(),
                          v.HasMember("parentId") ? v["parentId"].GetString()
                                                  : ""};
  if (type == "AgentError")
    return AgentError{v["agentId"].GetString(), v["message"].GetString(),
                      v.HasMember("parentId") ? v["parentId"].GetString() : ""};
  if (type == "AgentCompacting")
    return AgentCompacting{v["agentId"].GetString(),
                           v.HasMember("parentId") ? v["parentId"].GetString()
                                                   : ""};
  if (type == "AgentCompactionThinking")
    return AgentCompactionThinking{
        v["agentId"].GetString(), v["delta"].GetString(),
        v.HasMember("parentId") ? v["parentId"].GetString() : ""};
  if (type == "AgentCompactionText")
    return AgentCompactionText{
        v["agentId"].GetString(), v["delta"].GetString(),
        v.HasMember("parentId") ? v["parentId"].GetString() : ""};
  if (type == "ContextCompacted")
    return ContextCompacted{
        v["agentId"].GetString(), v["tokensSaved"].GetUint(),
        v.HasMember("parentId") ? v["parentId"].GetString() : ""};
  if (type == "AgentProcessOutput")
    return AgentProcessOutput{
        v["agentId"].GetString(),
        v["processId"].GetString(),
        v["output"].GetString(),
        v["isStderr"].GetBool(),
        v["finished"].GetBool(),
        v.HasMember("parentId") ? v["parentId"].GetString() : ""};
  throw std::runtime_error("Unknown EngineEvent type: " + type);
}

rapidjson::Document toJson(const ModelInfo &model) {
  rapidjson::Document d;
  d.SetObject();
  auto &a = d.GetAllocator();
  d.AddMember("id", rapidjson::Value(model.id.c_str(), a), a);
  d.AddMember("provider", rapidjson::Value(model.provider.c_str(), a), a);
  d.AddMember("contextWindow", model.contextWindow, a);
  rapidjson::Value mods(rapidjson::kArrayType);
  for (const auto &m : model.modalities)
    mods.PushBack(rapidjson::Value(m.c_str(), a), a);
  d.AddMember("modalities", mods, a);
  rapidjson::Value vars(rapidjson::kArrayType);
  for (const auto &v : model.variants) {
    rapidjson::Value variant(rapidjson::kObjectType);
    variant.AddMember("variantName", rapidjson::Value(v.variantName.c_str(), a),
                      a);
    variant.AddMember("extraMetadataJson",
                      rapidjson::Value(v.extraMetadataJson.c_str(), a), a);
    vars.PushBack(variant, a);
  }
  d.AddMember("variants", vars, a);
  d.AddMember("supportsReasoning", model.supportsReasoning, a);
  d.AddMember("pricePer1MInput", model.pricePer1MInput, a);
  d.AddMember("pricePer1MOutput", model.pricePer1MOutput, a);
  d.AddMember("pricePer1MCacheRead", model.pricePer1MCacheRead, a);
  d.AddMember("pricePer1MCacheWrite", model.pricePer1MCacheWrite, a);
  return d;
}

ModelInfo modelInfoFromJsonValue(const rapidjson::Value &v) {
  ModelInfo mi;
  mi.id = v["id"].GetString();
  mi.provider = v["provider"].GetString();
  if (v.HasMember("contextWindow"))
    mi.contextWindow = v["contextWindow"].GetUint();
  if (v.HasMember("modalities") && v["modalities"].IsArray()) {
    for (const auto &m : v["modalities"].GetArray())
      mi.modalities.push_back(m.GetString());
  }
  if (v.HasMember("variants") && v["variants"].IsArray()) {
    for (const auto &var : v["variants"].GetArray()) {
      ModelVariant mv;
      mv.variantName = var["variantName"].GetString();
      mv.extraMetadataJson = var["extraMetadataJson"].GetString();
      mi.variants.push_back(mv);
    }
  }
  if (v.HasMember("supportsReasoning"))
    mi.supportsReasoning = v["supportsReasoning"].GetBool();
  if (v.HasMember("pricePer1MInput"))
    mi.pricePer1MInput = v["pricePer1MInput"].GetDouble();
  if (v.HasMember("pricePer1MOutput"))
    mi.pricePer1MOutput = v["pricePer1MOutput"].GetDouble();
  if (v.HasMember("pricePer1MCacheRead"))
    mi.pricePer1MCacheRead = v["pricePer1MCacheRead"].GetDouble();
  if (v.HasMember("pricePer1MCacheWrite"))
    mi.pricePer1MCacheWrite = v["pricePer1MCacheWrite"].GetDouble();
  return mi;
}

rapidjson::Document toJson(const AgentConfig &config) {
  rapidjson::Document d;
  d.SetObject();
  auto &a = d.GetAllocator();
  d.AddMember("providerId", rapidjson::Value(config.providerId.c_str(), a), a);
  d.AddMember("modelId", rapidjson::Value(config.modelId.c_str(), a), a);
  d.AddMember("modelVariant", rapidjson::Value(config.modelVariant.c_str(), a),
              a);
  d.AddMember("personaName", rapidjson::Value(config.personaName.c_str(), a),
              a);
  d.AddMember("maxTurns", config.maxTurns, a);
  d.AddMember("temperature", config.temperature, a);
  if (config.maxTokens) {
    d.AddMember("maxTokens", config.maxTokens.value(), a);
  } else {
    d.AddMember("maxTokens", rapidjson::Value(rapidjson::kNullType), a);
  }
  rapidjson::Value stopSeqs(rapidjson::kArrayType);
  for (const auto &s : config.stop)
    stopSeqs.PushBack(rapidjson::Value(s.c_str(), a), a);
  d.AddMember("stop", stopSeqs, a);
  d.AddMember("persistHistory", config.persistHistory, a);
  return d;
}

AgentConfig agentConfigFromJsonValue(const rapidjson::Value &v) {
  AgentConfig cfg;
  if (v.HasMember("providerId"))
    cfg.providerId = v["providerId"].GetString();
  if (v.HasMember("modelId"))
    cfg.modelId = v["modelId"].GetString();
  if (v.HasMember("modelVariant"))
    cfg.modelVariant = v["modelVariant"].GetString();
  if (v.HasMember("personaName"))
    cfg.personaName = v["personaName"].GetString();
  if (v.HasMember("maxTurns"))
    cfg.maxTurns = v["maxTurns"].GetInt();
  if (v.HasMember("temperature"))
    cfg.temperature = v["temperature"].GetFloat();
  if (v.HasMember("maxTokens") && v["maxTokens"].IsUint())
    cfg.maxTokens = v["maxTokens"].GetUint();
  if (v.HasMember("stop") && v["stop"].IsArray()) {
    for (const auto &s : v["stop"].GetArray())
      cfg.stop.push_back(s.GetString());
  }
  if (v.HasMember("persistHistory"))
    cfg.persistHistory = v["persistHistory"].GetBool();
  return cfg;
}

std::string serializeToString(const AgentContext &ctx) {
  rapidjson::Document d = toJson(ctx);
  rapidjson::StringBuffer buffer;
  rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
  d.Accept(writer);
  return buffer.GetString();
}

AgentContext deserializeFromString(const std::string &json) {
  rapidjson::Document d;
  d.Parse(json.c_str());
  if (d.HasParseError())
    throw std::runtime_error("JSON Parse Error");
  return fromJson(d);
}

} // namespace firmius::shared

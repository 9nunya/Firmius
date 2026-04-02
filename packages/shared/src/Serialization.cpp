#include "Serialization.hpp"
#include "utils/StringUtil.hpp"

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

std::string messageVisibilityToString(MessageVisibility value) {
  switch (value) {
  case MessageVisibility::Visible:
    return "Visible";
  case MessageVisibility::Internal:
    return "Internal";
  }
  return "Visible";
}

MessageVisibility stringToMessageVisibility(const std::string &str) {
  if (str == "Visible")
    return MessageVisibility::Visible;
  if (str == "Internal")
    return MessageVisibility::Internal;
  throw std::runtime_error("Unknown MessageVisibility: " + str);
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
  case ToolScope::PlanRead:
    return "PlanRead";
  case ToolScope::PlanWrite:
    return "PlanWrite";
  case ToolScope::ChunkRead:
    return "ChunkRead";
  case ToolScope::ChunkWrite:
    return "ChunkWrite";
  case ToolScope::ChunkAssign:
    return "ChunkAssign";
  case ToolScope::ChunkReview:
    return "ChunkReview";
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
  if (str == "PlanRead")
    return ToolScope::PlanRead;
  if (str == "PlanWrite")
    return ToolScope::PlanWrite;
  if (str == "ChunkRead")
    return ToolScope::ChunkRead;
  if (str == "ChunkWrite")
    return ToolScope::ChunkWrite;
  if (str == "ChunkAssign")
    return ToolScope::ChunkAssign;
  if (str == "ChunkReview")
    return ToolScope::ChunkReview;
  throw std::runtime_error("Unknown ToolScope: " + str);
}

std::string threadPermissionModeToString(ThreadPermissionMode value) {
  switch (value) {
  case ThreadPermissionMode::Request:
    return "Request";
  case ThreadPermissionMode::AlwaysAllow:
    return "AlwaysAllow";
  case ThreadPermissionMode::DenyAll:
    return "DenyAll";
  }
  return "Unknown";
}

ThreadPermissionMode stringToThreadPermissionMode(const std::string &str) {
  if (str == "Request")
    return ThreadPermissionMode::Request;
  if (str == "AlwaysAllow")
    return ThreadPermissionMode::AlwaysAllow;
  if (str == "DenyAll")
    return ThreadPermissionMode::DenyAll;
  throw std::runtime_error("Unknown ThreadPermissionMode: " + str);
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

std::string planStatusToString(PlanStatus value) {
  switch (value) {
  case PlanStatus::Draft:
    return "Draft";
  case PlanStatus::Active:
    return "Active";
  case PlanStatus::Paused:
    return "Paused";
  case PlanStatus::Done:
    return "Done";
  case PlanStatus::Abandoned:
    return "Abandoned";
  }
  return "Draft";
}

PlanStatus stringToPlanStatus(const std::string &str) {
  if (str == "Draft")
    return PlanStatus::Draft;
  if (str == "Active")
    return PlanStatus::Active;
  if (str == "Paused")
    return PlanStatus::Paused;
  if (str == "Done")
    return PlanStatus::Done;
  if (str == "Abandoned")
    return PlanStatus::Abandoned;
  throw std::runtime_error("Unknown PlanStatus: " + str);
}

std::string workChunkStatusToString(WorkChunkStatus value) {
  switch (value) {
  case WorkChunkStatus::Ready:
    return "Ready";
  case WorkChunkStatus::InProgress:
    return "InProgress";
  case WorkChunkStatus::Implemented:
    return "Implemented";
  case WorkChunkStatus::Verifying:
    return "Verifying";
  case WorkChunkStatus::Done:
    return "Done";
  case WorkChunkStatus::Blocked:
    return "Blocked";
  case WorkChunkStatus::Failed:
    return "Failed";
  case WorkChunkStatus::Cancelled:
    return "Cancelled";
  }
  return "Ready";
}

WorkChunkStatus stringToWorkChunkStatus(const std::string &str) {
  if (str == "Draft")
    return WorkChunkStatus::Ready;
  if (str == "Ready")
    return WorkChunkStatus::Ready;
  if (str == "InProgress")
    return WorkChunkStatus::InProgress;
  if (str == "Implemented")
    return WorkChunkStatus::Implemented;
  if (str == "Verifying")
    return WorkChunkStatus::Verifying;
  if (str == "Done")
    return WorkChunkStatus::Done;
  if (str == "Blocked")
    return WorkChunkStatus::Blocked;
  if (str == "Failed")
    return WorkChunkStatus::Failed;
  if (str == "Cancelled")
    return WorkChunkStatus::Cancelled;
  throw std::runtime_error("Unknown WorkChunkStatus: " + str);
}

std::string todoStatusToString(TodoStatus value) {
  switch (value) {
  case TodoStatus::Pending:
    return "Pending";
  case TodoStatus::InProgress:
    return "InProgress";
  case TodoStatus::Done:
    return "Done";
  }
  return "Pending";
}

std::string noticeSeverityToString(NoticeSeverity value) {
  switch (value) {
  case NoticeSeverity::Info:
    return "info";
  case NoticeSeverity::Warning:
    return "warning";
  case NoticeSeverity::Error:
    return "error";
  case NoticeSeverity::Success:
    return "success";
  }
  return "info";
}

NoticeSeverity stringToNoticeSeverity(const std::string &str) {
  if (str == "info")
    return NoticeSeverity::Info;
  if (str == "warning")
    return NoticeSeverity::Warning;
  if (str == "error")
    return NoticeSeverity::Error;
  if (str == "success")
    return NoticeSeverity::Success;
  throw std::runtime_error("Unknown NoticeSeverity: " + str);
}

TodoStatus stringToTodoStatus(const std::string &str) {
  if (str == "Pending")
    return TodoStatus::Pending;
  if (str == "InProgress")
    return TodoStatus::InProgress;
  if (str == "Done")
    return TodoStatus::Done;
  throw std::runtime_error("Unknown TodoStatus: " + str);
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
  return {v.HasMember("startMs") && v["startMs"].IsUint64() ? v["startMs"].GetUint64() : 0,
          v.HasMember("firstTokenMs") && v["firstTokenMs"].IsUint64() ? v["firstTokenMs"].GetUint64() : 0,
          v.HasMember("endMs") && v["endMs"].IsUint64() ? v["endMs"].GetUint64() : 0,
          v.HasMember("toolExecutionMs") && v["toolExecutionMs"].IsUint64() ? v["toolExecutionMs"].GetUint64() : 0};
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
  return {v.HasMember("tokens") && v["tokens"].IsObject() ? tokenMetricsFromJson(v["tokens"]) : TokenMetrics{},
          v.HasMember("timing") && v["timing"].IsObject() ? timingMetricsFromJson(v["timing"]) : TimingMetrics{},
          v.HasMember("estimatedCostUsd") && v["estimatedCostUsd"].IsNumber() ? v["estimatedCostUsd"].GetDouble() : 0.0};
}

rapidjson::Value
hostCreationOptionsToJson(const HostCreationOptions &o,
                          rapidjson::Document::AllocatorType &a) {
  rapidjson::Value v(rapidjson::kObjectType);
  v.AddMember("type", rapidjson::Value(hostTypeToString(o.type).c_str(), a), a);
  v.AddMember("containerName", rapidjson::Value(o.containerName.c_str(), a), a);
  v.AddMember("connectToExisting", o.connectToExisting, a);
  v.AddMember("deleteOnExit", o.deleteOnExit, a);
  
  // Serialize volumeMounts array
  rapidjson::Value volumes(rapidjson::kArrayType);
  for (const auto& vol : o.volumeMounts) {
    volumes.PushBack(rapidjson::Value(vol.c_str(), a), a);
  }
  v.AddMember("volumeMounts", volumes, a);
  
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
  // Deserialize volumeMounts array
  if (v.HasMember("volumeMounts") && v["volumeMounts"].IsArray()) {
    for (const auto& vol : v["volumeMounts"].GetArray()) {
      if (vol.IsString()) {
        o.volumeMounts.push_back(vol.GetString());
      }
    }
  }
  return o;
}

rapidjson::Value workTaskToJson(const WorkTask &task,
                                rapidjson::Document::AllocatorType &a) {
  rapidjson::Value v(rapidjson::kObjectType);
  v.AddMember("id", rapidjson::Value(task.id.c_str(), a), a);
  v.AddMember("title", rapidjson::Value(task.title.c_str(), a), a);
  v.AddMember("goal", rapidjson::Value(task.goal.c_str(), a), a);
  v.AddMember("status",
              rapidjson::Value(workChunkStatusToString(task.status).c_str(), a),
              a);
  v.AddMember("notes", rapidjson::Value(task.notes.c_str(), a), a);
  v.AddMember("verification_condition",
              rapidjson::Value(task.verificationCondition.c_str(), a), a);
  v.AddMember("assigned_worker_id",
              rapidjson::Value(task.assignedWorkerId.c_str(), a), a);
  v.AddMember("created_at", task.createdAt, a);
  v.AddMember("updated_at", task.updatedAt, a);
  return v;
}

WorkTask workTaskFromJsonValue(const rapidjson::Value &v) {
  WorkTask task;
  task.id =
      v.HasMember("id") && v["id"].IsString() ? v["id"].GetString() : "";
  task.title =
      v.HasMember("title") && v["title"].IsString() ? v["title"].GetString() : "";
  task.goal =
      v.HasMember("goal") && v["goal"].IsString() ? v["goal"].GetString() : "";
  task.status = v.HasMember("status") && v["status"].IsString()
                    ? stringToWorkChunkStatus(v["status"].GetString())
                    : WorkChunkStatus::Ready;
  task.notes =
      v.HasMember("notes") && v["notes"].IsString() ? v["notes"].GetString() : "";
  task.verificationCondition =
      v.HasMember("verification_condition") && v["verification_condition"].IsString()
          ? v["verification_condition"].GetString()
          : "";
  task.assignedWorkerId =
      v.HasMember("assigned_worker_id") && v["assigned_worker_id"].IsString()
          ? v["assigned_worker_id"].GetString()
          : "";
  task.createdAt = v.HasMember("created_at") && v["created_at"].IsUint64()
                       ? v["created_at"].GetUint64()
                       : 0;
  task.updatedAt = v.HasMember("updated_at") && v["updated_at"].IsUint64()
                       ? v["updated_at"].GetUint64()
                       : 0;
  return task;
}

rapidjson::Value workChunkToJson(const WorkChunk &chunk,
                                 rapidjson::Document::AllocatorType &a) {
  rapidjson::Value v(rapidjson::kObjectType);
  v.AddMember("id", rapidjson::Value(chunk.id.c_str(), a), a);
  v.AddMember("title", rapidjson::Value(chunk.title.c_str(), a), a);
  v.AddMember("goal", rapidjson::Value(chunk.goal.c_str(), a), a);
  v.AddMember("context", rapidjson::Value(chunk.context.c_str(), a), a);
  v.AddMember("constraints", rapidjson::Value(chunk.constraints.c_str(), a), a);
  v.AddMember("completion", rapidjson::Value(chunk.completion.c_str(), a), a);
  v.AddMember("planning_gate", chunk.planningGate, a);
  v.AddMember("status",
              rapidjson::Value(workChunkStatusToString(chunk.status).c_str(), a),
              a);
  rapidjson::Value dependsOn(rapidjson::kArrayType);
  for (const auto &dependency : chunk.dependsOn) {
    dependsOn.PushBack(rapidjson::Value(dependency.c_str(), a), a);
  }
  v.AddMember("depends_on", dependsOn, a);
  v.AddMember("assigned_agent_id",
              rapidjson::Value(chunk.assignedAgentId.c_str(), a), a);
  v.AddMember("attempt_count", chunk.attemptCount, a);
  v.AddMember("result_summary",
              rapidjson::Value(chunk.resultSummary.c_str(), a), a);
  v.AddMember("review_summary",
              rapidjson::Value(chunk.reviewSummary.c_str(), a), a);
  v.AddMember("created_at", chunk.createdAt, a);
  v.AddMember("updated_at", chunk.updatedAt, a);

  // V2 richer chunk spec fields
  rapidjson::Value filesToRead(rapidjson::kArrayType);
  for (const auto &f : chunk.filesToRead) {
    filesToRead.PushBack(rapidjson::Value(f.c_str(), a), a);
  }
  v.AddMember("files_to_read", filesToRead, a);

  rapidjson::Value filesToTouch(rapidjson::kArrayType);
  for (const auto &f : chunk.filesToTouch) {
    filesToTouch.PushBack(rapidjson::Value(f.c_str(), a), a);
  }
  v.AddMember("files_to_touch", filesToTouch, a);

  v.AddMember("cwd", rapidjson::Value(chunk.cwd.c_str(), a), a);
  v.AddMember("verification_condition",
              rapidjson::Value(chunk.verificationCondition.c_str(), a), a);
  v.AddMember("handoff_notes",
              rapidjson::Value(chunk.handoffNotes.c_str(), a), a);

  // V2 task structure
  rapidjson::Value tasks(rapidjson::kArrayType);
  for (const auto &task : chunk.tasks) {
    tasks.PushBack(workTaskToJson(task, a), a);
  }
  v.AddMember("tasks", tasks, a);

  return v;
}

WorkChunk workChunkFromJsonValue(const rapidjson::Value &v) {
  WorkChunk chunk;
  chunk.id =
      v.HasMember("id") && v["id"].IsString() ? v["id"].GetString() : "";
  chunk.title =
      v.HasMember("title") && v["title"].IsString() ? v["title"].GetString() : "";
  chunk.goal =
      v.HasMember("goal") && v["goal"].IsString() ? v["goal"].GetString() : "";
  chunk.context = v.HasMember("context") && v["context"].IsString()
                      ? v["context"].GetString()
                      : "";
  chunk.constraints =
      v.HasMember("constraints") && v["constraints"].IsString()
          ? v["constraints"].GetString()
          : "";
  chunk.completion =
      v.HasMember("completion") && v["completion"].IsString()
          ? v["completion"].GetString()
          : "";
  chunk.planningGate =
      v.HasMember("planning_gate") && v["planning_gate"].IsBool()
          ? v["planning_gate"].GetBool()
          : (v.HasMember("planningGate") && v["planningGate"].IsBool()
                 ? v["planningGate"].GetBool()
                 : false);
  chunk.status = v.HasMember("status") && v["status"].IsString()
                     ? stringToWorkChunkStatus(v["status"].GetString())
                     : WorkChunkStatus::Ready;
  if (v.HasMember("depends_on") && v["depends_on"].IsArray()) {
    for (const auto &dependency : v["depends_on"].GetArray()) {
      if (dependency.IsString()) {
        chunk.dependsOn.push_back(dependency.GetString());
      }
    }
  } else if (v.HasMember("dependsOn") && v["dependsOn"].IsArray()) {
    for (const auto &dependency : v["dependsOn"].GetArray()) {
      if (dependency.IsString()) {
        chunk.dependsOn.push_back(dependency.GetString());
      }
    }
  }
  chunk.assignedAgentId =
      v.HasMember("assigned_agent_id") && v["assigned_agent_id"].IsString()
          ? v["assigned_agent_id"].GetString()
          : (v.HasMember("assignedAgentId") && v["assignedAgentId"].IsString()
                 ? v["assignedAgentId"].GetString()
                 : "");
  chunk.attemptCount = v.HasMember("attempt_count") && v["attempt_count"].IsInt()
                           ? v["attempt_count"].GetInt()
                           : (v.HasMember("attemptCount") &&
                                      v["attemptCount"].IsInt()
                                  ? v["attemptCount"].GetInt()
                                  : 0);
  chunk.resultSummary =
      v.HasMember("result_summary") && v["result_summary"].IsString()
          ? v["result_summary"].GetString()
          : (v.HasMember("resultSummary") && v["resultSummary"].IsString()
                 ? v["resultSummary"].GetString()
                 : "");
  chunk.reviewSummary =
      v.HasMember("review_summary") && v["review_summary"].IsString()
          ? v["review_summary"].GetString()
          : (v.HasMember("reviewSummary") && v["reviewSummary"].IsString()
                 ? v["reviewSummary"].GetString()
                 : "");
  chunk.createdAt = v.HasMember("created_at") && v["created_at"].IsUint64()
                        ? v["created_at"].GetUint64()
                        : (v.HasMember("createdAt") && v["createdAt"].IsUint64()
                               ? v["createdAt"].GetUint64()
                               : 0);
  chunk.updatedAt = v.HasMember("updated_at") && v["updated_at"].IsUint64()
                        ? v["updated_at"].GetUint64()
                        : (v.HasMember("updatedAt") && v["updatedAt"].IsUint64()
                               ? v["updatedAt"].GetUint64()
                               : 0);

  // V2 richer chunk spec fields
  if (v.HasMember("files_to_read") && v["files_to_read"].IsArray()) {
    for (const auto &f : v["files_to_read"].GetArray()) {
      if (f.IsString()) {
        chunk.filesToRead.push_back(f.GetString());
      }
    }
  }
  if (v.HasMember("files_to_touch") && v["files_to_touch"].IsArray()) {
    for (const auto &f : v["files_to_touch"].GetArray()) {
      if (f.IsString()) {
        chunk.filesToTouch.push_back(f.GetString());
      }
    }
  }
  chunk.cwd = v.HasMember("cwd") && v["cwd"].IsString() ? v["cwd"].GetString() : "";
  chunk.verificationCondition =
      v.HasMember("verification_condition") && v["verification_condition"].IsString()
          ? v["verification_condition"].GetString()
          : "";
  chunk.handoffNotes =
      v.HasMember("handoff_notes") && v["handoff_notes"].IsString()
          ? v["handoff_notes"].GetString()
          : "";

  // V2 task structure
  if (v.HasMember("tasks") && v["tasks"].IsArray()) {
    for (const auto &taskValue : v["tasks"].GetArray()) {
      if (taskValue.IsObject()) {
        chunk.tasks.push_back(workTaskFromJsonValue(taskValue));
      }
    }
  }

  return chunk;
}

rapidjson::Value planToJson(const Plan &plan,
                            rapidjson::Document::AllocatorType &a) {
  rapidjson::Value v(rapidjson::kObjectType);
  v.AddMember("id", rapidjson::Value(plan.id.c_str(), a), a);
  v.AddMember("thread_id", rapidjson::Value(plan.threadId.c_str(), a), a);
  v.AddMember("title", rapidjson::Value(plan.title.c_str(), a), a);
  v.AddMember("objective", rapidjson::Value(plan.objective.c_str(), a), a);
  v.AddMember("context", rapidjson::Value(plan.context.c_str(), a), a);
  v.AddMember("strategy", rapidjson::Value(plan.strategy.c_str(), a), a);
  v.AddMember("status",
              rapidjson::Value(planStatusToString(plan.status).c_str(), a), a);
  v.AddMember("notes", rapidjson::Value(plan.notes.c_str(), a), a);
  v.AddMember("created_at", plan.createdAt, a);
  v.AddMember("updated_at", plan.updatedAt, a);
  rapidjson::Value chunks(rapidjson::kArrayType);
  for (const auto &chunk : plan.chunks) {
    chunks.PushBack(workChunkToJson(chunk, a), a);
  }
  v.AddMember("chunks", chunks, a);
  return v;
}

Plan planFromJsonValue(const rapidjson::Value &v) {
  Plan plan;
  plan.id =
      v.HasMember("id") && v["id"].IsString() ? v["id"].GetString() : "";
  plan.threadId = v.HasMember("thread_id") && v["thread_id"].IsString()
                      ? v["thread_id"].GetString()
                      : (v.HasMember("threadId") && v["threadId"].IsString()
                             ? v["threadId"].GetString()
                             : "");
  plan.title =
      v.HasMember("title") && v["title"].IsString() ? v["title"].GetString() : "";
  plan.objective =
      v.HasMember("objective") && v["objective"].IsString()
          ? v["objective"].GetString()
          : "";
  plan.context = v.HasMember("context") && v["context"].IsString()
                     ? v["context"].GetString()
                     : "";
  plan.strategy =
      v.HasMember("strategy") && v["strategy"].IsString()
          ? v["strategy"].GetString()
          : "";
  plan.status = v.HasMember("status") && v["status"].IsString()
                    ? stringToPlanStatus(v["status"].GetString())
                    : PlanStatus::Draft;
  plan.notes =
      v.HasMember("notes") && v["notes"].IsString() ? v["notes"].GetString() : "";
  plan.createdAt = v.HasMember("created_at") && v["created_at"].IsUint64()
                       ? v["created_at"].GetUint64()
                       : (v.HasMember("createdAt") && v["createdAt"].IsUint64()
                              ? v["createdAt"].GetUint64()
                              : 0);
  plan.updatedAt = v.HasMember("updated_at") && v["updated_at"].IsUint64()
                       ? v["updated_at"].GetUint64()
                       : (v.HasMember("updatedAt") && v["updatedAt"].IsUint64()
                              ? v["updatedAt"].GetUint64()
                              : 0);
  if (v.HasMember("chunks") && v["chunks"].IsArray()) {
    for (const auto &chunkValue : v["chunks"].GetArray()) {
      if (chunkValue.IsObject()) {
        plan.chunks.push_back(workChunkFromJsonValue(chunkValue));
      }
    }
  }
  return plan;
}

rapidjson::Value todoItemToJson(const TodoItem &item,
                                rapidjson::Document::AllocatorType &a) {
  rapidjson::Value v(rapidjson::kObjectType);
  v.AddMember("id", item.id, a);
  v.AddMember("text", rapidjson::Value(item.text.c_str(), a), a);
  v.AddMember("status",
              rapidjson::Value(todoStatusToString(item.status).c_str(), a), a);
  v.AddMember("chunk_id", rapidjson::Value(item.chunkId.c_str(), a), a);
  v.AddMember("plan_id", rapidjson::Value(item.planId.c_str(), a), a);
  v.AddMember("created_at", item.createdAt, a);
  v.AddMember("updated_at", item.updatedAt, a);
  return v;
}

TodoItem todoItemFromJsonValue(const rapidjson::Value &v) {
  TodoItem item;
  item.id = v.HasMember("id") && v["id"].IsInt() ? v["id"].GetInt() : 0;
  item.text =
      v.HasMember("text") && v["text"].IsString() ? v["text"].GetString() : "";
  item.status = v.HasMember("status") && v["status"].IsString()
                    ? stringToTodoStatus(v["status"].GetString())
                    : TodoStatus::Pending;
  item.chunkId = v.HasMember("chunk_id") && v["chunk_id"].IsString()
                     ? v["chunk_id"].GetString()
                     : (v.HasMember("chunkId") && v["chunkId"].IsString()
                            ? v["chunkId"].GetString()
                            : "");
  item.planId = v.HasMember("plan_id") && v["plan_id"].IsString()
                    ? v["plan_id"].GetString()
                    : (v.HasMember("planId") && v["planId"].IsString()
                           ? v["planId"].GetString()
                           : "");
  item.createdAt = v.HasMember("created_at") && v["created_at"].IsUint64()
                       ? v["created_at"].GetUint64()
                       : (v.HasMember("createdAt") && v["createdAt"].IsUint64()
                              ? v["createdAt"].GetUint64()
                              : 0);
  item.updatedAt = v.HasMember("updated_at") && v["updated_at"].IsUint64()
                       ? v["updated_at"].GetUint64()
                       : (v.HasMember("updatedAt") && v["updatedAt"].IsUint64()
                              ? v["updatedAt"].GetUint64()
                              : 0);
  return item;
}

rapidjson::Value agentTodoListToJson(const AgentTodoList &list,
                                     rapidjson::Document::AllocatorType &a) {
  rapidjson::Value v(rapidjson::kObjectType);
  v.AddMember("thread_id", rapidjson::Value(list.threadId.c_str(), a), a);
  v.AddMember("agent_id", rapidjson::Value(list.agentId.c_str(), a), a);
  v.AddMember("next_id", list.nextId, a);
  rapidjson::Value items(rapidjson::kArrayType);
  for (const auto &item : list.items) {
    items.PushBack(todoItemToJson(item, a), a);
  }
  v.AddMember("items", items, a);
  return v;
}

AgentTodoList agentTodoListFromJsonValue(const rapidjson::Value &v) {
  AgentTodoList list;
  list.threadId = v.HasMember("thread_id") && v["thread_id"].IsString()
                      ? v["thread_id"].GetString()
                      : (v.HasMember("threadId") && v["threadId"].IsString()
                             ? v["threadId"].GetString()
                             : "");
  list.agentId = v.HasMember("agent_id") && v["agent_id"].IsString()
                     ? v["agent_id"].GetString()
                     : (v.HasMember("agentId") && v["agentId"].IsString()
                            ? v["agentId"].GetString()
                            : "");
  list.nextId = v.HasMember("next_id") && v["next_id"].IsInt()
                    ? v["next_id"].GetInt()
                    : (v.HasMember("nextId") && v["nextId"].IsInt()
                           ? v["nextId"].GetInt()
                           : 1);
  if (list.nextId <= 0) {
    list.nextId = 1;
  }
  if (v.HasMember("items") && v["items"].IsArray()) {
    for (const auto &itemValue : v["items"].GetArray()) {
      if (itemValue.IsObject()) {
        list.items.push_back(todoItemFromJsonValue(itemValue));
      }
    }
  }
  return list;
}

rapidjson::Value
threadArtifactMetadataToJsonValue(const ThreadArtifactMetadata &metadata,
                                  rapidjson::Document::AllocatorType &a) {
  rapidjson::Value v(rapidjson::kObjectType);
  v.AddMember("thread_id", rapidjson::Value(metadata.threadId.c_str(), a), a);
  v.AddMember("owner_agent_id",
              rapidjson::Value(metadata.ownerAgentId.c_str(), a), a);
  v.AddMember("owner_friendly_name",
              rapidjson::Value(metadata.ownerFriendlyName.c_str(), a), a);
  v.AddMember("filename", rapidjson::Value(metadata.filename.c_str(), a), a);
  v.AddMember("storage_path", rapidjson::Value(metadata.storagePath.c_str(), a),
              a);
  v.AddMember("created_at", metadata.createdAt, a);
  v.AddMember("updated_at", metadata.updatedAt, a);
  if (metadata.kind.has_value()) {
    v.AddMember("kind", rapidjson::Value(metadata.kind->c_str(), a), a);
  } else {
    v.AddMember("kind", rapidjson::Value(rapidjson::kNullType), a);
  }
  if (metadata.description.has_value()) {
    v.AddMember("description",
                rapidjson::Value(metadata.description->c_str(), a), a);
  } else {
    v.AddMember("description", rapidjson::Value(rapidjson::kNullType), a);
  }
  return v;
}

ThreadArtifactMetadata
threadArtifactMetadataFromJsonValue(const rapidjson::Value &v) {
  ThreadArtifactMetadata metadata;
  metadata.threadId =
      v.HasMember("thread_id") && v["thread_id"].IsString()
          ? v["thread_id"].GetString()
          : (v.HasMember("threadId") && v["threadId"].IsString()
                 ? v["threadId"].GetString()
                 : "");
  metadata.ownerAgentId =
      v.HasMember("owner_agent_id") && v["owner_agent_id"].IsString()
          ? v["owner_agent_id"].GetString()
          : (v.HasMember("ownerAgentId") && v["ownerAgentId"].IsString()
                 ? v["ownerAgentId"].GetString()
                 : "");
  metadata.ownerFriendlyName =
      v.HasMember("owner_friendly_name") && v["owner_friendly_name"].IsString()
          ? v["owner_friendly_name"].GetString()
          : (v.HasMember("ownerFriendlyName") &&
                     v["ownerFriendlyName"].IsString()
                 ? v["ownerFriendlyName"].GetString()
                 : "");
  metadata.filename =
      v.HasMember("filename") && v["filename"].IsString()
          ? v["filename"].GetString()
          : "";
  metadata.storagePath =
      v.HasMember("storage_path") && v["storage_path"].IsString()
          ? v["storage_path"].GetString()
          : (v.HasMember("storagePath") && v["storagePath"].IsString()
                 ? v["storagePath"].GetString()
                 : "");
  metadata.createdAt =
      v.HasMember("created_at") && v["created_at"].IsUint64()
          ? v["created_at"].GetUint64()
          : (v.HasMember("createdAt") && v["createdAt"].IsUint64()
                 ? v["createdAt"].GetUint64()
                 : 0);
  metadata.updatedAt =
      v.HasMember("updated_at") && v["updated_at"].IsUint64()
          ? v["updated_at"].GetUint64()
          : (v.HasMember("updatedAt") && v["updatedAt"].IsUint64()
                 ? v["updatedAt"].GetUint64()
                 : 0);
  if (v.HasMember("kind") && v["kind"].IsString()) {
    metadata.kind = v["kind"].GetString();
  }
  if (v.HasMember("description") && v["description"].IsString()) {
    metadata.description = v["description"].GetString();
  }
  return metadata;
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
    v.AddMember("signature", rapidjson::Value(thk->signature.c_str(), a), a);
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
  } else if (auto *notice = std::get_if<NoticeContent>(&p)) {
    v.AddMember("type", "notice", a);
    v.AddMember("title", rapidjson::Value(notice->title.c_str(), a), a);
    v.AddMember("message", rapidjson::Value(notice->message.c_str(), a), a);
    v.AddMember("details", rapidjson::Value(notice->details.c_str(), a), a);
    v.AddMember("severity",
                rapidjson::Value(noticeSeverityToString(notice->severity).c_str(),
                                 a),
                a);
  }
  return v;
}

MessagePart messagePartFromJson(const rapidjson::Value &v) {
  std::string type = v["type"].GetString();
  if (type == "text")
    return TextContent{v["text"].GetString()};
  if (type == "thinking") {
    std::string signature = v.HasMember("signature") ? v["signature"].GetString() : "";
    return ThinkingContent{v["thinking"].GetString(), signature};
  }
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
  if (type == "notice")
    return NoticeContent{v["title"].GetString(), v["message"].GetString(),
                         v["details"].GetString(),
                         v.HasMember("severity") && v["severity"].IsString()
                             ? stringToNoticeSeverity(v["severity"].GetString())
                             : NoticeSeverity::Info};
  // Unknown or missing type: return a safe default instead of throwing
  return TextContent{};
}

rapidjson::Value messageToJson(const Message &m,
                               rapidjson::Document::AllocatorType &a) {
  rapidjson::Value v(rapidjson::kObjectType);
  v.AddMember("id", rapidjson::Value(m.id.c_str(), a), a);
  v.AddMember("role", rapidjson::Value(roleToString(m.role).c_str(), a), a);
  v.AddMember("visibility",
              rapidjson::Value(
                  messageVisibilityToString(m.visibility).c_str(), a),
              a);
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
  if (v.HasMember("visibility") && v["visibility"].IsString()) {
    m.visibility = stringToMessageVisibility(v["visibility"].GetString());
  }
  for (const auto &p : v["content"].GetArray())
    m.content.push_back(messagePartFromJson(p));
  m.timestamp = v["timestamp"].GetUint64();
  if (v["parentId"].IsString())
    m.parentId = v["parentId"].GetString();
  return m;
}

const char *agentOutcomeKindToString(AgentOutcome::Kind kind) {
  switch (kind) {
  case AgentOutcome::Kind::Response:
    return "Response";
  case AgentOutcome::Kind::NoSummary:
    return "NoSummary";
  case AgentOutcome::Kind::Cancelled:
    return "Cancelled";
  case AgentOutcome::Kind::Failed:
    return "Failed";
  }
  return "Failed";
}

rapidjson::Value agentOutcomeToJson(const AgentOutcome &outcome,
                                    rapidjson::Document::AllocatorType &a) {
  rapidjson::Value v(rapidjson::kObjectType);
  v.AddMember("kind",
              rapidjson::Value(agentOutcomeKindToString(outcome.kind), a).Move(),
              a);
  v.AddMember("text", rapidjson::Value(outcome.text.c_str(), a).Move(), a);
  rapidjson::Value created(rapidjson::kArrayType);
  for (const auto &artifact : outcome.artifacts_created) {
    created.PushBack(threadArtifactMetadataToJsonValue(artifact, a), a);
  }
  v.AddMember("artifacts_created", created, a);
  rapidjson::Value updated(rapidjson::kArrayType);
  for (const auto &artifact : outcome.artifacts_updated) {
    updated.PushBack(threadArtifactMetadataToJsonValue(artifact, a), a);
  }
  v.AddMember("artifacts_updated", updated, a);
  return v;
}

AgentOutcome agentOutcomeFromJson(const rapidjson::Value &v) {
  if (!v.IsObject()) {
    throw std::runtime_error("AgentOutcome must be an object");
  }
  if (!v.HasMember("kind") || !v["kind"].IsString()) {
    throw std::runtime_error("AgentOutcome.kind must be a string");
  }
  if (!v.HasMember("text") || !v["text"].IsString()) {
    throw std::runtime_error("AgentOutcome.text must be a string");
  }
  const std::string kind = v["kind"].GetString();
  const std::string text = v["text"].GetString();
  AgentOutcome outcome;
  outcome.text = text;
  if (v.HasMember("artifacts_created") && v["artifacts_created"].IsArray()) {
    for (const auto &artifact : v["artifacts_created"].GetArray()) {
      if (artifact.IsObject()) {
        outcome.artifacts_created.push_back(
            threadArtifactMetadataFromJsonValue(artifact));
      }
    }
  }
  if (v.HasMember("artifacts_updated") && v["artifacts_updated"].IsArray()) {
    for (const auto &artifact : v["artifacts_updated"].GetArray()) {
      if (artifact.IsObject()) {
        outcome.artifacts_updated.push_back(
            threadArtifactMetadataFromJsonValue(artifact));
      }
    }
  }
  if (kind == "Response") {
    outcome.kind = AgentOutcome::Kind::Response;
    return outcome;
  }
  if (kind == "NoSummary") {
    outcome.kind = AgentOutcome::Kind::NoSummary;
    return outcome;
  }
  if (kind == "Cancelled") {
    outcome.kind = AgentOutcome::Kind::Cancelled;
    return outcome;
  }
  if (kind == "Failed") {
    outcome.kind = AgentOutcome::Kind::Failed;
    return outcome;
  }
  throw std::runtime_error("Unknown AgentOutcome kind: " + kind);
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
  // Identity section (defensive)
  if (v.HasMember("identity") && v["identity"].IsObject()) {
    const auto &id = v["identity"];
    if (id.HasMember("id") && id["id"].IsString())
      ctx.identity.id = id["id"].GetString();
    if (id.HasMember("name") && id["name"].IsString())
      ctx.identity.name = id["name"].GetString();
    if (id.HasMember("role") && id["role"].IsString())
      ctx.identity.role = id["role"].GetString();
    if (id.HasMember("goal") && id["goal"].IsString())
      ctx.identity.goal = id["goal"].GetString();
    if (id.HasMember("systemPrompt") && id["systemPrompt"].IsString())
      ctx.identity.systemPrompt = id["systemPrompt"].GetString();
    if (id.HasMember("parentId") && id["parentId"].IsString())
      ctx.identity.parentId = id["parentId"].GetString();
    if (id.HasMember("friendlyName") && id["friendlyName"].IsString())
      ctx.identity.friendlyName = id["friendlyName"].GetString();
  }
  // Permissions section (defensive)
  if (v.HasMember("permissions") && v["permissions"].IsObject()) {
    const auto &perm = v["permissions"];
    if (perm.HasMember("allowedScopes") && perm["allowedScopes"].IsArray()) {
      for (const auto &s : perm["allowedScopes"].GetArray())
        if (s.IsString()) ctx.permissions.allowedScopes.push_back(stringToToolScope(s.GetString()));
    }
    if (perm.HasMember("allowedPaths") && perm["allowedPaths"].IsArray()) {
      for (const auto &p : perm["allowedPaths"].GetArray())
        if (p.IsString()) ctx.permissions.allowedPaths.push_back(p.GetString());
    }
    if (perm.HasMember("allowOutsideCwd") && perm["allowOutsideCwd"].IsBool())
      ctx.permissions.allowOutsideCwd = perm["allowOutsideCwd"].GetBool();
  }
  // Environment section (defensive)
  if (v.HasMember("environment") && v["environment"].IsObject()) {
    const auto &env = v["environment"];
    if (env.HasMember("type") && env["type"].IsString())
      ctx.environment.type = stringToHostType(env["type"].GetString());
    if (env.HasMember("identifier") && env["identifier"].IsString())
      ctx.environment.identifier = env["identifier"].GetString();
    if (env.HasMember("cwd") && env["cwd"].IsString())
      ctx.environment.cwd = env["cwd"].GetString();
    if (env.HasMember("envVars") && env["envVars"].IsObject()) {
      for (auto it = env["envVars"].MemberBegin();
           it != env["envVars"].MemberEnd(); ++it)
        if (it->value.IsString()) ctx.environment.envVars[it->name.GetString()] = it->value.GetString();
    }
  }
  // History section (defensive)
  ctx.history = std::make_shared<AgentHistory>();
  if (v.HasMember("history") && v["history"].IsObject()) {
    const auto &hist = v["history"];
    if (hist.HasMember("threadId") && hist["threadId"].IsString())
      ctx.history->threadId = hist["threadId"].GetString();
    if (hist.HasMember("turns") && hist["turns"].IsArray()) {
      for (const auto &t : hist["turns"].GetArray()) {
        if (!t.IsObject()) continue;
        AgentTurn turn;
        if (t.HasMember("turnId") && t["turnId"].IsString())
          turn.turnId = t["turnId"].GetString();
        if (t.HasMember("messages") && t["messages"].IsArray()) {
          for (const auto &m : t["messages"].GetArray())
            turn.messages.push_back(messageFromJson(m));
        }
        if (t.HasMember("metrics") && t["metrics"].IsObject())
          turn.metrics = agentMetricsFromJson(t["metrics"]);
        if (t.HasMember("stopReason") && t["stopReason"].IsString()) {
          turn.stopReason = stringToStopReason(t["stopReason"].GetString());
        }
        ctx.history->turns.push_back(turn);
      }
    }
  }
  // State section (defensive)
  if (v.HasMember("state") && v["state"].IsObject()) {
    const auto &state = v["state"];
    if (state.HasMember("currentStatus") && state["currentStatus"].IsString())
      ctx.state.currentStatus = stringToAgentStatus(state["currentStatus"].GetString());
    if (state.HasMember("pendingToolCalls") && state["pendingToolCalls"].IsArray()) {
      for (const auto &p : state["pendingToolCalls"].GetArray())
        if (p.IsString()) ctx.state.pendingToolCalls.push_back(p.GetString());
    }
    if (state.HasMember("ownedProcesses") && state["ownedProcesses"].IsArray()) {
      for (const auto &p : state["ownedProcesses"].GetArray())
        if (p.IsString()) ctx.state.ownedProcesses.push_back(p.GetString());
    }
    if (state.HasMember("readFiles") && state["readFiles"].IsArray()) {
      for (const auto &f : state["readFiles"].GetArray())
        if (f.IsString()) ctx.state.readFiles.push_back(f.GetString());
    }
    if (state.HasMember("fullyReadFiles") && state["fullyReadFiles"].IsArray()) {
      for (const auto &f : state["fullyReadFiles"].GetArray())
        if (f.IsString()) ctx.state.fullyReadFiles.push_back(f.GetString());
    }
    if (state.HasMember("editedFiles") && state["editedFiles"].IsArray()) {
      for (const auto &f : state["editedFiles"].GetArray())
        if (f.IsString()) ctx.state.editedFiles.push_back(f.GetString());
    }
    if (state.HasMember("completedActions") && state["completedActions"].IsArray()) {
      for (const auto &act : state["completedActions"].GetArray())
        if (act.IsString()) ctx.state.completedActions.push_back(act.GetString());
    }
    if (state.HasMember("fatalError") && state["fatalError"].IsString())
      ctx.state.fatalError = state["fatalError"].GetString();
    // Load blockingProcessIds (new format) or currentBlockingProcessId (legacy)
    if (state.HasMember("blockingProcessIds") && state["blockingProcessIds"].IsArray()) {
      for (const auto &pid : state["blockingProcessIds"].GetArray()) {
        if (pid.IsString()) ctx.state.blockingProcessIds.push_back(pid.GetString());
      }
    } else if (state.HasMember("currentBlockingProcessId") &&
               state["currentBlockingProcessId"].IsString()) {
      ctx.state.blockingProcessIds.push_back(
          state["currentBlockingProcessId"].GetString());
    }
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

  if (v.HasMember("aggregateMetrics") && v["aggregateMetrics"].IsObject())
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
    d.AddMember("exitCode", pod->exitCode, a);
    d.AddMember("durationMs", pod->durationMs, a);
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
                              v["finished"].GetBool(),
                              v.HasMember("exitCode") ? v["exitCode"].GetInt()
                                                       : -1,
                              v.HasMember("durationMs")
                                  ? v["durationMs"].GetDouble()
                                  : 0.0};
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
  d.AddMember("is_benchmark_run", m.isBenchmarkRun, a);
  d.AddMember("benchmark_id", rapidjson::Value(m.benchmarkId.c_str(), a), a);
  d.AddMember("benchmark_task_id",
              rapidjson::Value(m.benchmarkTaskId.c_str(), a), a);
  d.AddMember("active_plan_id", rapidjson::Value(m.activePlanId.c_str(), a), a);
  if (m.lastRetryableRequest.has_value()) {
    rapidjson::Value retry(rapidjson::kObjectType);
    retry.AddMember(
        "targetAgentId",
        rapidjson::Value(m.lastRetryableRequest->targetAgentId.c_str(), a), a);
    retry.AddMember("turnId",
                    rapidjson::Value(m.lastRetryableRequest->turnId.c_str(), a),
                    a);
    retry.AddMember("text",
                    rapidjson::Value(m.lastRetryableRequest->text.c_str(), a),
                    a);
    rapidjson::Value images(rapidjson::kArrayType);
    for (const auto &image : m.lastRetryableRequest->images) {
      images.PushBack(messagePartToJson(image, a), a);
    }
    retry.AddMember("images", images, a);
    retry.AddMember("recordedAt", m.lastRetryableRequest->recordedAt, a);
    retry.AddMember("eligible", m.lastRetryableRequest->eligible, a);
    d.AddMember("lastRetryableRequest", retry, a);
  } else {
    d.AddMember("lastRetryableRequest", rapidjson::Value(rapidjson::kNullType),
                a);
  }
  d.AddMember(
      "permissionMode",
      rapidjson::Value(threadPermissionModeToString(m.permissionMode).c_str(), a),
      a);
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
  m.isBenchmarkRun =
      v.HasMember("is_benchmark_run") && v["is_benchmark_run"].IsBool()
          ? v["is_benchmark_run"].GetBool()
          : (v.HasMember("isBenchmarkRun") && v["isBenchmarkRun"].IsBool()
                 ? v["isBenchmarkRun"].GetBool()
                 : false);
  m.benchmarkId =
      v.HasMember("benchmark_id") && v["benchmark_id"].IsString()
          ? v["benchmark_id"].GetString()
          : (v.HasMember("benchmarkId") && v["benchmarkId"].IsString()
                 ? v["benchmarkId"].GetString()
                 : "");
  m.benchmarkTaskId =
      v.HasMember("benchmark_task_id") && v["benchmark_task_id"].IsString()
          ? v["benchmark_task_id"].GetString()
          : (v.HasMember("benchmarkTaskId") && v["benchmarkTaskId"].IsString()
                 ? v["benchmarkTaskId"].GetString()
                 : "");
  m.activePlanId =
      v.HasMember("active_plan_id") && v["active_plan_id"].IsString()
          ? v["active_plan_id"].GetString()
          : (v.HasMember("activePlanId") && v["activePlanId"].IsString()
                 ? v["activePlanId"].GetString()
                 : "");
  if (v.HasMember("lastRetryableRequest") &&
      v["lastRetryableRequest"].IsObject()) {
    ThreadMetadata::RetryableRequest retry;
    const auto &retryValue = v["lastRetryableRequest"];
    retry.targetAgentId =
        retryValue.HasMember("targetAgentId") &&
                retryValue["targetAgentId"].IsString()
            ? retryValue["targetAgentId"].GetString()
            : "";
    retry.turnId = retryValue.HasMember("turnId") && retryValue["turnId"].IsString()
                       ? retryValue["turnId"].GetString()
                       : "";
    retry.text = retryValue.HasMember("text") && retryValue["text"].IsString()
                     ? retryValue["text"].GetString()
                     : "";
    retry.recordedAt =
        retryValue.HasMember("recordedAt") && retryValue["recordedAt"].IsUint64()
            ? retryValue["recordedAt"].GetUint64()
            : 0;
    retry.eligible =
        retryValue.HasMember("eligible") && retryValue["eligible"].IsBool()
            ? retryValue["eligible"].GetBool()
            : false;
    if (retryValue.HasMember("images") && retryValue["images"].IsArray()) {
      for (const auto &imageValue : retryValue["images"].GetArray()) {
        if (!imageValue.IsObject() || !imageValue.HasMember("type") ||
            !imageValue["type"].IsString()) {
          continue;
        }
        try {
          auto part = messagePartFromJson(imageValue);
          if (auto *image = std::get_if<ImageContent>(&part)) {
            retry.images.push_back(*image);
          }
        } catch (...) {
        }
      }
    }
    if (!retry.text.empty() || !retry.images.empty()) {
      m.lastRetryableRequest = std::move(retry);
    }
  }
  m.permissionMode =
      v.HasMember("permissionMode") && v["permissionMode"].IsString()
          ? stringToThreadPermissionMode(v["permissionMode"].GetString())
          : ThreadPermissionMode::Request;
  m.createdAt = v.HasMember("createdAt") && v["createdAt"].IsUint64()
                    ? v["createdAt"].GetUint64()
                    : 0;
  m.lastActiveAt = v.HasMember("lastActiveAt") && v["lastActiveAt"].IsUint64()
                       ? v["lastActiveAt"].GetUint64()
                       : 0;
  return m;
}

rapidjson::Document toJson(const WorkChunk &chunk) {
  rapidjson::Document d;
  d.SetObject();
  auto &a = d.GetAllocator();
  d.CopyFrom(workChunkToJson(chunk, a), a);
  return d;
}

WorkChunk workChunkFromJson(const rapidjson::Value &value) {
  return workChunkFromJsonValue(value);
}

rapidjson::Document toJson(const Plan &plan) {
  rapidjson::Document d;
  d.SetObject();
  auto &a = d.GetAllocator();
  d.CopyFrom(planToJson(plan, a), a);
  return d;
}

Plan planFromJson(const rapidjson::Value &value) {
  return planFromJsonValue(value);
}

rapidjson::Document toJson(const TodoItem &item) {
  rapidjson::Document d;
  d.SetObject();
  auto &a = d.GetAllocator();
  d.CopyFrom(todoItemToJson(item, a), a);
  return d;
}

TodoItem todoItemFromJson(const rapidjson::Value &value) {
  return todoItemFromJsonValue(value);
}

rapidjson::Document toJson(const AgentTodoList &list) {
  rapidjson::Document d;
  d.SetObject();
  auto &a = d.GetAllocator();
  d.CopyFrom(agentTodoListToJson(list, a), a);
  return d;
}

AgentTodoList agentTodoListFromJson(const rapidjson::Value &value) {
  return agentTodoListFromJsonValue(value);
}

rapidjson::Document toJson(const ThreadArtifactMetadata &metadata) {
  rapidjson::Document d;
  d.SetObject();
  auto &a = d.GetAllocator();
  d.CopyFrom(threadArtifactMetadataToJsonValue(metadata, a), a);
  return d;
}

ThreadArtifactMetadata
threadArtifactMetadataFromJson(const rapidjson::Value &value) {
  return threadArtifactMetadataFromJsonValue(value);
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
  } else if (auto *fe = std::get_if<AgentFileEdited>(&ev)) {
    d.AddMember("type", "AgentFileEdited", a);
    d.AddMember("agentId", rapidjson::Value(fe->agentId.c_str(), a), a);
    d.AddMember("parentId", rapidjson::Value(fe->parentId.c_str(), a), a);
    d.AddMember("path", rapidjson::Value(fe->path.c_str(), a), a);
    d.AddMember("toolCallId", rapidjson::Value(fe->toolCallId.c_str(), a), a);
    d.AddMember("diffPreview", rapidjson::Value(fe->diffPreview.c_str(), a), a);
    d.AddMember("addedLines", fe->addedLines, a);
    d.AddMember("removedLines", fe->removedLines, a);
  } else if (auto *tc = std::get_if<AgentTurnCompleted>(&ev)) {
    d.AddMember("type", "AgentTurnCompleted", a);
    d.AddMember("agentId", rapidjson::Value(tc->agentId.c_str(), a), a);
    d.AddMember("turn", toJson(tc->turn).Move(), a);
    d.AddMember("aggregateMetrics", toJson(tc->aggregateMetrics).Move(), a);
    d.AddMember("parentId", rapidjson::Value(tc->parentId.c_str(), a), a);
  } else if (auto *f = std::get_if<AgentFinished>(&ev)) {
    d.AddMember("type", "AgentFinished", a);
    d.AddMember("agentId", rapidjson::Value(f->agentId.c_str(), a), a);
    d.AddMember("outcome", agentOutcomeToJson(f->outcome, a), a);
    d.AddMember("parentId", rapidjson::Value(f->parentId.c_str(), a), a);
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
    d.AddMember("exitCode", apo->exitCode, a);
    d.AddMember("durationMs", apo->durationMs, a);
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
  if (type == "AgentFileEdited")
    return AgentFileEdited{
        v["agentId"].GetString(),
        v.HasMember("parentId") ? v["parentId"].GetString() : "",
        v.HasMember("path") ? v["path"].GetString() : "",
        v.HasMember("toolCallId") ? v["toolCallId"].GetString() : "",
        v.HasMember("diffPreview") ? v["diffPreview"].GetString() : "",
        v.HasMember("addedLines") && v["addedLines"].IsInt()
            ? v["addedLines"].GetInt()
            : 0,
        v.HasMember("removedLines") && v["removedLines"].IsInt()
            ? v["removedLines"].GetInt()
            : 0};
  if (type == "AgentTurnCompleted")
    return AgentTurnCompleted{
        v["agentId"].GetString(), agentTurnFromJsonValue(v["turn"]),
        agentMetricsFromJsonValue(v["aggregateMetrics"]),
        v.HasMember("parentId") ? v["parentId"].GetString() : ""};
  if (type == "AgentFinished") {
    if (!v.HasMember("outcome") || !v["outcome"].IsObject()) {
      throw std::runtime_error("AgentFinished requires an outcome object");
    }
    AgentOutcome outcome = agentOutcomeFromJson(v["outcome"]);
    return AgentFinished{v["agentId"].GetString(), outcome,
                         v.HasMember("parentId") ? v["parentId"].GetString()
                                                 : ""};
  }
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
        v.HasMember("exitCode") ? v["exitCode"].GetInt() : -1,
        v.HasMember("durationMs") ? v["durationMs"].GetDouble() : 0.0,
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
  d.AddMember("maxOutputTokens", model.maxOutputTokens, a);
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
  if (v.HasMember("maxOutputTokens"))
    mi.maxOutputTokens = v["maxOutputTokens"].GetUint();
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

// WorkTask serialization externs
rapidjson::Document toJson(const WorkTask &task) {
  rapidjson::Document d;
  d.SetObject();
  auto &a = d.GetAllocator();
  rapidjson::Value v = workTaskToJson(task, a);
  for (auto &m : v.GetObject()) {
    d.AddMember(m.name, m.value, a);
  }
  return d;
}

WorkTask workTaskFromJson(const rapidjson::Value &v) {
  return workTaskFromJsonValue(v);
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

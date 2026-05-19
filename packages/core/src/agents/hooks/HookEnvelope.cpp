#include "agents/hooks/HookEnvelope.hpp"

#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

namespace firmius::core::hooks {

namespace {

void addStr(rapidjson::Value &obj, const char *key, const std::string &value,
            rapidjson::Document::AllocatorType &alloc) {
  obj.AddMember(rapidjson::Value(key, alloc).Move(),
                rapidjson::Value(value.c_str(), alloc).Move(), alloc);
}

void addRawJson(rapidjson::Value &obj, const char *key,
                const std::string &json,
                rapidjson::Document::AllocatorType &alloc) {
  if (json.empty()) {
    obj.AddMember(rapidjson::Value(key, alloc).Move(),
                  rapidjson::Value(rapidjson::kNullType).Move(), alloc);
    return;
  }
  rapidjson::Document inner(&alloc);
  if (inner.Parse(json.c_str()).HasParseError()) {
    // Pass through as a raw string when the upstream payload was not JSON.
    obj.AddMember(rapidjson::Value(key, alloc).Move(),
                  rapidjson::Value(json.c_str(), alloc).Move(), alloc);
    return;
  }
  obj.AddMember(rapidjson::Value(key, alloc).Move(), inner.Move(), alloc);
}

} // namespace

std::string serializeEnvelope(const HookEnvelope &env) {
  rapidjson::Document doc;
  doc.SetObject();
  auto &alloc = doc.GetAllocator();

  addStr(doc, "hook_id", env.hookId, alloc);
  addStr(doc, "event", env.hookEvent, alloc);
  addStr(doc, "firmius_version", env.firmiusVersion, alloc);

  rapidjson::Value payload(rapidjson::kObjectType);
  addStr(payload, "thread_id", env.threadId, alloc);
  addStr(payload, "agent_id", env.agentId, alloc);
  addStr(payload, "persona", env.persona, alloc);
  addStr(payload, "active_mode", env.activeMode, alloc);
  addStr(payload, "tool", env.toolName, alloc);
  addRawJson(payload, "tool_args", env.toolArgsJson, alloc);
  addRawJson(payload, "tool_result", env.toolResultJson, alloc);
  if (env.toolSuccess.has_value()) {
    payload.AddMember("tool_success", *env.toolSuccess, alloc);
  } else {
    payload.AddMember("tool_success",
                      rapidjson::Value(rapidjson::kNullType).Move(), alloc);
  }
  addStr(payload, "user_message", env.userMessage, alloc);
  addStr(payload, "from_mode", env.fromMode, alloc);
  addStr(payload, "to_mode", env.toMode, alloc);
  addStr(payload, "completed_workflow", env.completedWorkflowId, alloc);
  addStr(payload, "subagent_branch", env.subagentBranchId, alloc);
  addRawJson(payload, "return_payload", env.returnPayloadJson, alloc);

  rapidjson::Value extras(rapidjson::kObjectType);
  for (const auto &[k, v] : env.extra) {
    addStr(extras, k.c_str(), v, alloc);
  }
  payload.AddMember("extra", extras, alloc);

  doc.AddMember("payload", payload, alloc);
  addRawJson(doc, "state", env.stateSnapshotJson, alloc);

  doc.AddMember("claude_code_compat", env.claudeCodeCompat, alloc);

  rapidjson::StringBuffer sb;
  rapidjson::Writer<rapidjson::StringBuffer> w(sb);
  doc.Accept(w);
  return std::string(sb.GetString(), sb.GetSize());
}

HookEnvelope buildEnvelope(const std::string &hookId, WorkflowEventKind kind,
                           const EventPayload &payload,
                           const std::string &stateSnapshotJson) {
  HookEnvelope env;
  env.hookId = hookId;
  env.hookEvent = workflowEventKindToString(kind);
  // Filled by build system via -DFIRMIUS_VERSION_STRING when wired; until
  // then the env reads "dev" so tests can match deterministically.
  env.firmiusVersion = "dev";

  env.threadId = payload.threadId;
  env.agentId = payload.agentId;
  env.persona = payload.persona;
  env.activeMode = payload.activeMode;
  env.toolName = payload.toolName;
  env.toolArgsJson = payload.toolArgsJson;
  env.toolResultJson = payload.toolResultJson;
  env.toolSuccess = payload.toolSuccess;
  env.userMessage = payload.userMessage;
  env.fromMode = payload.fromMode;
  env.toMode = payload.toMode;
  env.completedWorkflowId = payload.completedWorkflowId;
  env.subagentBranchId = payload.subagentBranchId;
  env.returnPayloadJson = payload.returnPayloadJson;
  env.extra = payload.extra;
  env.stateSnapshotJson =
      stateSnapshotJson.empty() ? "{}" : stateSnapshotJson;
  return env;
}

HookOutcome parseHookOutcome(const std::string &hookId,
                             WorkflowEventKind eventKind, int exitCode,
                             const std::string &stdoutBuf,
                             const std::string &stderrBuf,
                             bool claudeCodeCompat) {
  HookOutcome out;
  out.tags["hook_id"] = hookId;
  out.tags["exit_code"] = std::to_string(exitCode);

  // Try JSON parse first when stdout looks like an object/array. Hooks
  // that opt in to the structured channel emit a JSON object whose shape
  // is documented in the README.
  bool parsedAsJson = false;
  if (!stdoutBuf.empty()) {
    const char first = stdoutBuf.front();
    if (first == '{' || first == '[') {
      rapidjson::Document doc;
      if (!doc.Parse(stdoutBuf.c_str()).HasParseError() && doc.IsObject()) {
        parsedAsJson = true;
        if (doc.HasMember("decision") && doc["decision"].IsString()) {
          const std::string d = doc["decision"].GetString();
          if (d == "block")
            out.decision = HookOutcome::Decision::Block;
          else if (d == "replace")
            out.decision = HookOutcome::Decision::Replace;
          else
            out.decision = HookOutcome::Decision::Allow;
        }
        if (doc.HasMember("reason") && doc["reason"].IsString()) {
          out.blockReason = doc["reason"].GetString();
        }
        if (doc.HasMember("reminder") && doc["reminder"].IsString()) {
          out.reminderForAgent = doc["reminder"].GetString();
        }
        if (doc.HasMember("outcome") && doc["outcome"].IsString()) {
          out.outcomeLabel = doc["outcome"].GetString();
        }
        if (doc.HasMember("replacement_args") &&
            doc["replacement_args"].IsObject()) {
          rapidjson::StringBuffer sb;
          rapidjson::Writer<rapidjson::StringBuffer> w(sb);
          doc["replacement_args"].Accept(w);
          out.replacementToolArgs =
              std::string(sb.GetString(), sb.GetSize());
        }
        if (doc.HasMember("state_writes") && doc["state_writes"].IsArray()) {
          for (const auto &w : doc["state_writes"].GetArray()) {
            if (!w.IsObject())
              continue;
            HookOutcome::StateWrite sw;
            if (w.HasMember("scope") && w["scope"].IsString())
              sw.scope = w["scope"].GetString();
            if (w.HasMember("path") && w["path"].IsString())
              sw.path = w["path"].GetString();
            if (w.HasMember("value")) {
              rapidjson::StringBuffer sb;
              rapidjson::Writer<rapidjson::StringBuffer> wr(sb);
              w["value"].Accept(wr);
              sw.valueJson = std::string(sb.GetString(), sb.GetSize());
            }
            out.stateWrites.push_back(std::move(sw));
          }
        }
      }
    }
  }

  // Legacy + Claude Code conventions for shell hooks that didn't emit
  // structured JSON:
  //   - exit 0 with non-empty stdout → reminder text injected.
  //   - exit 2 (Claude Code) → block with stderr-or-stdout as reason.
  //   - any other non-zero → soft fail; reminder injected, no block.
  if (!parsedAsJson) {
    if (exitCode == 0) {
      if (!stdoutBuf.empty()) {
        out.reminderForAgent =
            "<FIRMIUS_HOOK id=\"" + hookId + "\" event=\"" +
            workflowEventKindToString(eventKind) + "\" exit=\"0\">\n" +
            stdoutBuf + "</FIRMIUS_HOOK>";
      }
    } else if (claudeCodeCompat && exitCode == 2) {
      out.decision = HookOutcome::Decision::Block;
      out.blockReason = stderrBuf.empty() ? stdoutBuf : stderrBuf;
      if (!out.blockReason.empty()) {
        out.reminderForAgent = "<FIRMIUS_HOOK id=\"" + hookId +
                               "\" event=\"" +
                               workflowEventKindToString(eventKind) +
                               "\" exit=\"2\">\n" + out.blockReason +
                               "</FIRMIUS_HOOK>";
      }
    } else {
      const std::string &payload = stderrBuf.empty() ? stdoutBuf : stderrBuf;
      if (!payload.empty()) {
        out.reminderForAgent = "<FIRMIUS_HOOK id=\"" + hookId +
                               "\" event=\"" +
                               workflowEventKindToString(eventKind) +
                               "\" exit=\"" + std::to_string(exitCode) +
                               "\">\n" + payload + "</FIRMIUS_HOOK>";
      }
    }
  }

  return out;
}

} // namespace firmius::core::hooks

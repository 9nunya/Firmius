#include "tools/ProcessTool.hpp"

#include "AgentRegistry.hpp"
#include "IAgent.hpp"
#include "agents/AgentPermissionChecks.hpp"
#include "utils/FSUtil.hpp"
#include "utils/TerminalUtil.hpp"
#include <chrono>
#include <cctype>
#include <iomanip>
#include <optional>
#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>
#include <thread>

namespace firmius::core {
using namespace firmius::shared;

namespace {

shared::ToolResult failWithStructuredData(const rapidjson::Document &d,
                                          const std::string &error) {
  shared::ToolResult result = shared::ToolResult::fail(error);
  rapidjson::StringBuffer buffer;
  rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
  d.Accept(writer);
  result.data = buffer.GetString();
  return result;
}

std::string escapeForJson(const std::string& s) {
    std::string result;
    result.reserve(s.size());
    for (char c : s) {
        switch (c) {
            case '"': result += "\\\""; break;
            case '\\': result += "\\\\"; break;
            case '\n': result += "\\n"; break;
            case '\r': result += "\\r"; break;
            case '\t': result += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 32) {
                    std::ostringstream oss;
                    oss << "\\u" << std::hex << std::setw(4) << std::setfill('0') << static_cast<int>(static_cast<unsigned char>(c));
                    result += oss.str();
                } else {
                    result += c;
                }
        }
    }
    return result;
}

shared::ToolResult executeExecute(const rapidjson::Value &input, shared::ToolContext &ctx) {
  std::string command;
  if (input.HasMember("command") && input["command"].IsString()) {
    command = input["command"].GetString();
  } else {
    return shared::ToolResult::fail("Missing required field: command");
  }

  std::string cwd;
  if (input.HasMember("cwd") && input["cwd"].IsString()) {
    cwd = input["cwd"].GetString();
  }

  int timeout_ms = 15000;
  if (input.HasMember("timeout_ms") && input["timeout_ms"].IsInt()) {
    timeout_ms = input["timeout_ms"].GetInt();
  }

  std::string processId;
  shared::ProcessSnapshot snap;
  try {
    std::string normalizedCommand;
    normalizedCommand.reserve(command.size());
    bool previousWasSpace = false;
    for (char ch : command) {
      if (std::isspace(static_cast<unsigned char>(ch))) {
        if (!previousWasSpace) {
          normalizedCommand.push_back(' ');
          previousWasSpace = true;
        }
      } else {
        normalizedCommand.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
        previousWasSpace = false;
      }
    }
    if (!normalizedCommand.empty() && normalizedCommand.front() == ' ') normalizedCommand.erase(normalizedCommand.begin());
    if (!normalizedCommand.empty() && normalizedCommand.back() == ' ') normalizedCommand.pop_back();

    const bool mentionsApplyPatch =
        normalizedCommand == "apply_patch" ||
        normalizedCommand.rfind("apply_patch ", 0) == 0 ||
        normalizedCommand.find(" apply_patch ") != std::string::npos ||
        normalizedCommand.find("| apply_patch") != std::string::npos ||
        normalizedCommand.find("&& apply_patch") != std::string::npos ||
        normalizedCommand.find("|| apply_patch") != std::string::npos;
    if (mentionsApplyPatch) {
      return shared::ToolResult::fail("Foreign tool conflict: 'apply_patch' is not available in Firmius and must not be run via Process.execute. Use Files (Read) + Edit (patch) instead.");
    }

    std::string effectiveCwd = cwd.empty() ? ctx.agent.getContext().environment.cwd : cwd;
    effectiveCwd = ctx.agent.getEnvironment()->getWorkspace().resolvePath(effectiveCwd);

    ctx.agent.getPermissions()->validatePathAccess(effectiveCwd, firmius::shared::AccessMode::READ);
    auto intent = ctx.agent.getPermissions()->getIntentAnalyzer().analyze(command, effectiveCwd);
    auto approval = ctx.agent.getPermissions()->requestCommandApproval(command, intent, "Process");
    if (approval == PermissionResponse::Deny) {
      return shared::ToolResult::fail("Command execution denied: " + command);
    }

    processId = ctx.agent.getEnvironment()->getProcessManager().spawnProcess(command, ctx.currentToolCallId, effectiveCwd, {}, true);
    ctx.agent.getEnvironment()->getProcessManager().addBlockingProcessId(processId);

    auto start = std::chrono::steady_clock::now();
    auto sleepDuration = std::chrono::milliseconds(1);
    const auto maxSleep = std::chrono::milliseconds(20);

    while (true) {
      if (ctx.cancelRequested()) {
        snap = ctx.agent.getEnvironment()->getProcessManager().inspectProcess(processId);
        ctx.agent.getEnvironment()->getProcessManager().killProcess(processId);
        ctx.agent.getEnvironment()->getProcessManager().removeBlockingProcessId(processId);

        rapidjson::Document doc; doc.SetObject(); auto &a = doc.GetAllocator();
        doc.AddMember("exit_code", -2, a);
        doc.AddMember("stdout", rapidjson::Value(snap.stdoutData.c_str(), a).Move(), a);
        doc.AddMember("stderr", rapidjson::Value(snap.stderrData.c_str(), a).Move(), a);
        doc.AddMember("duration_ms", snap.elapsedMs, a);
        doc.AddMember("finish_reason", "Interrupted", a);
        doc.AddMember("command_success", false, a);
        doc.AddMember("process_id", rapidjson::Value(processId.c_str(), a).Move(), a);
        return shared::ToolResult::ok(doc, processId);
      }

      snap = ctx.agent.getEnvironment()->getProcessManager().inspectProcess(processId);
      if (!snap.running) break;

      auto now = std::chrono::steady_clock::now();
      auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - start).count();
      if (elapsed > timeout_ms) {
        ctx.agent.getEnvironment()->getProcessManager().removeBlockingProcessId(processId);
        rapidjson::Document doc; doc.SetObject(); auto &a = doc.GetAllocator();
        doc.AddMember("exit_code", -1, a);
        doc.AddMember("stdout", rapidjson::Value(snap.stdoutData.c_str(), a).Move(), a);
        doc.AddMember("stderr", rapidjson::Value(snap.stderrData.c_str(), a).Move(), a);
        doc.AddMember("duration_ms", snap.elapsedMs, a);
        doc.AddMember("finish_reason", "Timeout", a);
        doc.AddMember("command_success", false, a);
        doc.AddMember("process_id", rapidjson::Value(processId.c_str(), a).Move(), a);
        doc.AddMember("note", rapidjson::Value("The command is still running in the background. Use Process.status to check its current output or Process.wait to wait for its completion.", a).Move(), a);
        auto result = shared::ToolResult::ok(doc, processId);
        result.is_background = true;
        return result;
      }

      if (!ctx.waitFor(sleepDuration)) {
        ctx.agent.getEnvironment()->getProcessManager().removeBlockingProcessId(processId);
        rapidjson::Document doc; doc.SetObject(); auto &a = doc.GetAllocator();
        doc.AddMember("exit_code", -2, a);
        doc.AddMember("stdout", rapidjson::Value(snap.stdoutData.c_str(), a).Move(), a);
        doc.AddMember("stderr", rapidjson::Value(snap.stderrData.c_str(), a).Move(), a);
        doc.AddMember("duration_ms", snap.elapsedMs, a);
        doc.AddMember("finish_reason", "Interrupted", a);
        doc.AddMember("command_success", false, a);
        doc.AddMember("process_id", rapidjson::Value(processId.c_str(), a).Move(), a);
        return shared::ToolResult::ok(doc, processId);
      }
      if (sleepDuration < maxSleep) sleepDuration = std::min(maxSleep, sleepDuration * 2);
    }

    ctx.agent.getEnvironment()->getProcessManager().removeBlockingProcessId(processId);
    rapidjson::Document doc; doc.SetObject(); auto &a = doc.GetAllocator();
    doc.AddMember("exit_code", snap.exitCode, a);
    doc.AddMember("stdout", rapidjson::Value(snap.stdoutData.c_str(), a).Move(), a);
    doc.AddMember("stderr", rapidjson::Value(snap.stderrData.c_str(), a).Move(), a);
    doc.AddMember("duration_ms", snap.elapsedMs, a);
    doc.AddMember("finish_reason", "Natural", a);
    doc.AddMember("command_success", snap.exitCode == 0, a);
    doc.AddMember("process_id", rapidjson::Value(processId.c_str(), a).Move(), a);

    if (snap.exitCode != 0) {
      return failWithStructuredData(doc, "Command exited with non-zero exit code: " + std::to_string(snap.exitCode));
    }
    return shared::ToolResult::ok(doc, processId);
  } catch (const std::exception &e) {
    if (!processId.empty()) ctx.agent.getEnvironment()->getProcessManager().removeBlockingProcessId(processId);
    return shared::ToolResult::fail(e.what());
  }
}

shared::ToolResult executeSpawn(const rapidjson::Value &input, shared::ToolContext &ctx) {
  std::string command;
  if (input.HasMember("command") && input["command"].IsString()) {
    command = input["command"].GetString();
  } else {
    return shared::ToolResult::fail("Missing required field: command");
  }

  std::string cwd;
  if (input.HasMember("cwd") && input["cwd"].IsString()) {
    cwd = input["cwd"].GetString();
  }

  try {
    std::string effectiveCwd = cwd.empty() ? ctx.agent.getContext().environment.cwd : cwd;
    effectiveCwd = ctx.agent.getEnvironment()->getWorkspace().resolvePath(effectiveCwd);

    ctx.agent.getPermissions()->validatePathAccess(effectiveCwd, firmius::shared::AccessMode::READ);
    auto intent = ctx.agent.getPermissions()->getIntentAnalyzer().analyze(command, effectiveCwd);
    auto approval = ctx.agent.getPermissions()->requestCommandApproval(command, intent, "Process");
    if (approval == PermissionResponse::Deny) {
      return shared::ToolResult::fail("Command execution denied: " + command);
    }

    std::string processId = ctx.agent.getEnvironment()->getProcessManager().spawnProcess(command, ctx.currentToolCallId, effectiveCwd, {});

    rapidjson::Document doc; doc.SetObject();
    doc.AddMember("process_id", rapidjson::Value(processId.c_str(), doc.GetAllocator()).Move(), doc.GetAllocator());
    return shared::ToolResult::ok(doc, processId);
  } catch (const std::exception &e) {
    return shared::ToolResult::fail(e.what());
  }
}

shared::ToolResult executeStatus(const rapidjson::Value &input, shared::ToolContext &ctx) {
  std::string process_id;
  if (input.HasMember("process_id") && input["process_id"].IsString()) {
    process_id = input["process_id"].GetString();
  } else {
    return shared::ToolResult::fail("Missing required field: process_id");
  }

  try {
    if (AgentRegistry::instance().getAgent(process_id)) {
      return shared::ToolResult::fail("ID '" + process_id + "' belongs to a subagent, not a process. Use Delegate.wait (or Delegate.stop) for agent IDs.");
    }

    auto snapshot = ctx.agent.getEnvironment()->getProcessManager().inspectProcess(process_id);
    rapidjson::Document doc; doc.SetObject(); auto& a = doc.GetAllocator();
    doc.AddMember("isRunning", snapshot.running, a);
    doc.AddMember("exitCode", snapshot.exitCode, a);
    doc.AddMember("stdout", rapidjson::Value(snapshot.stdoutData.c_str(), a).Move(), a);
    doc.AddMember("stderr", rapidjson::Value(snapshot.stderrData.c_str(), a).Move(), a);
    doc.AddMember("duration_ms", snapshot.elapsedMs, a);
    return shared::ToolResult::ok(doc);
  } catch (const std::exception& e) {
    return shared::ToolResult::fail(e.what());
  }
}

shared::ToolResult executeWait(const rapidjson::Value &input, shared::ToolContext &ctx) {
  std::string process_id;
  if (input.HasMember("process_id") && input["process_id"].IsString()) {
    process_id = input["process_id"].GetString();
  } else {
    return shared::ToolResult::fail("Missing required field: process_id");
  }

  std::string pattern;
  if (input.HasMember("pattern") && input["pattern"].IsString()) {
    pattern = input["pattern"].GetString();
  }

  int timeout_ms = 30000;
  if (input.HasMember("timeout_ms") && input["timeout_ms"].IsInt()) {
    timeout_ms = input["timeout_ms"].GetInt();
  }

  try {
    auto startTime = std::chrono::steady_clock::now();
    while (true) {
      if (ctx.cancelRequested()) return shared::ToolResult::fail("Interrupted");
      auto snapshot = ctx.agent.getEnvironment()->getProcessManager().inspectProcess(process_id);
      bool patternFound = false;
      if (!pattern.empty()) {
        if (snapshot.stdoutData.find(pattern) != std::string::npos || snapshot.stderrData.find(pattern) != std::string::npos) patternFound = true;
      }
      if (!snapshot.running || patternFound) {
        rapidjson::Document doc; doc.SetObject(); auto& a = doc.GetAllocator();
        doc.AddMember("isRunning", snapshot.running, a);
        doc.AddMember("exitCode", snapshot.exitCode, a);
        doc.AddMember("stdout", rapidjson::Value(snapshot.stdoutData.c_str(), a).Move(), a);
        doc.AddMember("stderr", rapidjson::Value(snapshot.stderrData.c_str(), a).Move(), a);
        doc.AddMember("patternFound", patternFound, a);
        return shared::ToolResult::ok(doc);
      }
      auto now = std::chrono::steady_clock::now();
      auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - startTime).count();
      if (elapsed >= timeout_ms) return shared::ToolResult::fail("Timeout reached while waiting for process " + process_id);
      if (!ctx.waitFor(std::chrono::milliseconds(25))) return shared::ToolResult::fail("Interrupted");
    }
  } catch (const std::exception& e) {
    return shared::ToolResult::fail(e.what());
  }
}

shared::ToolResult executeInput(const rapidjson::Value &input, shared::ToolContext &ctx) {
  std::string process_id;
  if (input.HasMember("process_id") && input["process_id"].IsString()) {
    process_id = input["process_id"].GetString();
  } else {
    return shared::ToolResult::fail("Missing required field: process_id");
  }

  std::string text;
  if (input.HasMember("input") && input["input"].IsString()) {
    text = input["input"].GetString();
  } else {
    return shared::ToolResult::fail("Missing required field: input");
  }

  try {
    std::string translated = shared::TerminalUtil::translate(text);
    int linesSent = 0;
    int charsSent = 0;
    size_t last = 0;
    size_t next = 0;
    while ((next = translated.find('\n', last)) != std::string::npos) {
      std::string part = translated.substr(last, next - last + 1);
      ctx.agent.getEnvironment()->getProcessManager().writeToProcess(process_id, part);
      linesSent++;
      charsSent += static_cast<int>(part.size());
      auto start = std::chrono::steady_clock::now();
      while (std::chrono::steady_clock::now() - start < std::chrono::milliseconds(1000)) {
        if (ctx.cancelRequested()) return shared::ToolResult::fail("Interrupted");
        if (!ctx.waitFor(std::chrono::milliseconds(10))) return shared::ToolResult::fail("Interrupted");
      }
      last = next + 1;
    }
    if (last < translated.size()) {
      std::string remaining = translated.substr(last);
      ctx.agent.getEnvironment()->getProcessManager().writeToProcess(process_id, remaining);
      charsSent += static_cast<int>(remaining.size());
    }
    std::string escapedInput = escapeForJson(translated);
    std::string resultJson = "{\"sent\":\"" + escapedInput + "\",\"chars\":" + std::to_string(charsSent) + ",\"lines\":" + std::to_string(linesSent) + "}";
    return shared::ToolResult::ok(resultJson);
  } catch (const std::exception& e) {
    return shared::ToolResult::fail(e.what());
  }
}

} // namespace

shared::ToolMetadata ProcessTool::getMetadata() const {
  return {"Process", "Process operations. Use action Execute, Spawn, Status, Wait, or Input.", shared::ToolScope::Process};
}

std::shared_ptr<shared::JSONSchema> ProcessTool::getSchema() const {
  return shared::zObject({
      {"action", shared::zEnum({"Execute", "Spawn", "Status", "Wait", "Input"})->describe("Process operation to execute")},
      {"command", shared::zString()->setOptional()},
      {"process_id", shared::zString()->setOptional()},
      {"input", shared::zString()->setOptional()},
      {"pattern", shared::zString()->setOptional()},
      {"cwd", shared::zString()->setOptional()},
      {"timeout_ms", shared::zInteger()->setOptional()},
  });
}

shared::ToolResult ProcessTool::execute(const rapidjson::Value &input, shared::ToolContext &ctx) {
  if (!input.IsObject() || !input.HasMember("action") || !input["action"].IsString()) {
    return shared::ToolResult::fail("Process.action must be a string (Execute, Spawn, Status, Wait, or Input)");
  }
  const std::string action = input["action"].GetString();
  if (action == "Execute") return executeExecute(input, ctx);
  if (action == "Spawn") return executeSpawn(input, ctx);
  if (action == "Status") return executeStatus(input, ctx);
  if (action == "Wait") return executeWait(input, ctx);
  if (action == "Input") return executeInput(input, ctx);
  return shared::ToolResult::fail("Process.action must be Execute, Spawn, Status, Wait, or Input");
}

} // namespace firmius::core

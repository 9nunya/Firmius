#include "tools/ProcessTool.hpp"

#include "AgentRegistry.hpp"
#include "IAgent.hpp"
#include "agents/AgentPermissionChecks.hpp"
#include "utils/FSUtil.hpp"
#include "utils/TerminalUtil.hpp"
#include <chrono>
#include <cctype>
#include <algorithm>
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

rapidjson::Value makeString(const std::string &value,
                            rapidjson::Document::AllocatorType &alloc) {
  rapidjson::Value result;
  result.SetString(value.c_str(), static_cast<rapidjson::SizeType>(value.size()), alloc);
  return result;
}

std::string sliceOutput(const std::string &data, int fromOffset, int maxBytes,
                        int tailLines) {
  size_t start = fromOffset > 0
                     ? std::min(static_cast<size_t>(fromOffset), data.size())
                     : 0;
  if (tailLines > 0 && !data.empty()) {
    size_t pos = data.size();
    int seen = 0;
    while (pos > 0 && seen < tailLines) {
      --pos;
      if (data[pos] == '\n') {
        ++seen;
        if (seen == tailLines) {
          if (pos + 1 < data.size()) {
            ++pos;
          }
          break;
        }
      }
    }
    if (seen < tailLines) {
      pos = 0;
    }
    start = std::max(start, pos);
  }
  size_t count = data.size() - start;
  if (maxBytes > 0) {
    count = std::min(count, static_cast<size_t>(maxBytes));
  }
  return data.substr(start, count);
}

void addSnapshotFields(rapidjson::Document &doc,
                       const shared::ProcessSnapshot &snapshot,
                       const std::string &processId) {
  auto &a = doc.GetAllocator();
  doc.AddMember("process_id", makeString(processId, a), a);
  doc.AddMember("system_id", makeString(snapshot.systemId, a), a);
  doc.AddMember("isRunning", snapshot.running, a);
  doc.AddMember("running", snapshot.running, a);
  doc.AddMember("exitCode", snapshot.exitCode, a);
  doc.AddMember("exit_code", snapshot.exitCode, a);
  doc.AddMember("stdout", makeString(snapshot.stdoutData, a), a);
  doc.AddMember("stderr", makeString(snapshot.stderrData, a), a);
  doc.AddMember("stdout_bytes", static_cast<uint64_t>(snapshot.stdoutData.size()), a);
  doc.AddMember("stderr_bytes", static_cast<uint64_t>(snapshot.stderrData.size()), a);
  doc.AddMember("duration_ms", snapshot.elapsedMs, a);
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
        doc.AddMember("system_id", rapidjson::Value(snap.systemId.c_str(), a).Move(), a);
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
    auto snapshot = ctx.agent.getEnvironment()->getProcessManager().inspectProcess(processId);

    rapidjson::Document doc; doc.SetObject();
    doc.AddMember("process_id", rapidjson::Value(processId.c_str(), doc.GetAllocator()).Move(), doc.GetAllocator());
    doc.AddMember("system_id", rapidjson::Value(snapshot.systemId.c_str(), doc.GetAllocator()).Move(), doc.GetAllocator());
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
    rapidjson::Document doc; doc.SetObject();
    addSnapshotFields(doc, snapshot, process_id);
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
        addSnapshotFields(doc, snapshot, process_id);
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

shared::ToolResult executeKill(const rapidjson::Value &input, shared::ToolContext &ctx) {
  std::string process_id;
  if (input.HasMember("process_id") && input["process_id"].IsString()) {
    process_id = input["process_id"].GetString();
  } else {
    return shared::ToolResult::fail("Missing required field: process_id");
  }

  try {
    auto before = ctx.agent.getEnvironment()->getProcessManager().inspectProcess(process_id);
    ctx.agent.getEnvironment()->getProcessManager().killProcess(process_id);
    auto after = ctx.agent.getEnvironment()->getProcessManager().inspectProcess(process_id);
    auto start = std::chrono::steady_clock::now();
    while (after.running &&
           std::chrono::steady_clock::now() - start < std::chrono::milliseconds(1000)) {
      if (!ctx.waitFor(std::chrono::milliseconds(25))) {
        break;
      }
      after = ctx.agent.getEnvironment()->getProcessManager().inspectProcess(process_id);
    }
    rapidjson::Document doc; doc.SetObject(); auto &a = doc.GetAllocator();
    addSnapshotFields(doc, after, process_id);
    doc.AddMember("was_running", before.running, a);
    doc.AddMember("killed", before.running && !after.running, a);
    return shared::ToolResult::ok(doc);
  } catch (const std::exception& e) {
    return shared::ToolResult::fail(e.what());
  }
}

shared::ToolResult executeOutput(const rapidjson::Value &input, shared::ToolContext &ctx) {
  std::string process_id;
  if (input.HasMember("process_id") && input["process_id"].IsString()) {
    process_id = input["process_id"].GetString();
  } else {
    return shared::ToolResult::fail("Missing required field: process_id");
  }

  int fromOffset = 0;
  if (input.HasMember("from_offset") && input["from_offset"].IsInt()) {
    fromOffset = input["from_offset"].GetInt();
  }
  int maxBytes = 0;
  if (input.HasMember("max_bytes") && input["max_bytes"].IsInt()) {
    maxBytes = input["max_bytes"].GetInt();
  }
  int tailLines = 0;
  if (input.HasMember("tail_lines") && input["tail_lines"].IsInt()) {
    tailLines = input["tail_lines"].GetInt();
  }
  std::string stream = "combined";
  if (input.HasMember("stream") && input["stream"].IsString()) {
    stream = input["stream"].GetString();
  }

  try {
    auto snapshot = ctx.agent.getEnvironment()->getProcessManager().inspectProcess(process_id);
    const std::string combined = snapshot.stdoutData + snapshot.stderrData;
    const std::string *source = &combined;
    if (stream == "stdout") {
      source = &snapshot.stdoutData;
    } else if (stream == "stderr") {
      source = &snapshot.stderrData;
    } else if (stream != "combined") {
      return shared::ToolResult::fail("Process.Output stream must be stdout, stderr, or combined");
    }

    std::string output = sliceOutput(*source, fromOffset, maxBytes, tailLines);
    rapidjson::Document doc; doc.SetObject(); auto &a = doc.GetAllocator();
    doc.AddMember("process_id", makeString(process_id, a), a);
    doc.AddMember("system_id", makeString(snapshot.systemId, a), a);
    doc.AddMember("isRunning", snapshot.running, a);
    doc.AddMember("running", snapshot.running, a);
    doc.AddMember("stream", makeString(stream, a), a);
    doc.AddMember("output", makeString(output, a), a);
    doc.AddMember("from_offset", fromOffset, a);
    doc.AddMember("next_offset", static_cast<uint64_t>(source->size()), a);
    doc.AddMember("stdout_bytes", static_cast<uint64_t>(snapshot.stdoutData.size()), a);
    doc.AddMember("stderr_bytes", static_cast<uint64_t>(snapshot.stderrData.size()), a);
    doc.AddMember("truncated", maxBytes > 0 && output.size() == static_cast<size_t>(maxBytes), a);
    return shared::ToolResult::ok(doc);
  } catch (const std::exception& e) {
    return shared::ToolResult::fail(e.what());
  }
}

shared::ToolResult executeList(const rapidjson::Value &, shared::ToolContext &ctx) {
  rapidjson::Document doc; doc.SetObject(); auto &a = doc.GetAllocator();
  rapidjson::Value processes(rapidjson::kArrayType);
  for (const auto &processId : ctx.agent.getEnvironment()->getProcessManager().getProcessIds()) {
    try {
      auto snapshot = ctx.agent.getEnvironment()->getProcessManager().inspectProcess(processId);
      rapidjson::Value process(rapidjson::kObjectType);
      process.AddMember("process_id", makeString(processId, a), a);
      process.AddMember("system_id", makeString(snapshot.systemId, a), a);
      process.AddMember("isRunning", snapshot.running, a);
      process.AddMember("running", snapshot.running, a);
      process.AddMember("exitCode", snapshot.exitCode, a);
      process.AddMember("exit_code", snapshot.exitCode, a);
      process.AddMember("duration_ms", snapshot.elapsedMs, a);
      process.AddMember("stdout_bytes", static_cast<uint64_t>(snapshot.stdoutData.size()), a);
      process.AddMember("stderr_bytes", static_cast<uint64_t>(snapshot.stderrData.size()), a);
      processes.PushBack(process, a);
    } catch (...) {
    }
  }
  doc.AddMember("processes", processes, a);
  return shared::ToolResult::ok(doc);
}

} // namespace

shared::ToolMetadata ProcessTool::getMetadata() const {
  return {"Process",
          R"(Process operations for running commands and interacting with live child processes.

USAGE GUIDANCE:
- Use Execute for a blocking command when you want a final exit code/result in one step.
- Use Spawn for long-running commands, then follow with Status/Output/Wait/Input/Kill as needed.
- If you Spawn a process, you are expected to eventually Wait, Kill, or otherwise settle it; do not leave ghost processes behind.
- Never use Process as an editing tunnel for writing repository files; use Edit/EditWrite/EditReplace/EditRange for file modifications.
- Prefer narrow verification commands that produce binary evidence (exit code / exact output).

ACTIONS:
- Execute: run a command and wait for completion.
- Spawn: start a background process and get a process_id.
- Status: inspect running/completed state.
- Wait: wait for completion or a matching output condition.
- Input: send stdin to a live process.
- Output: read stdout/stderr slices from a live process.
- List: inspect known managed processes.
- Kill: terminate a managed process.
)",
          shared::ToolScope::Process};
}

std::shared_ptr<shared::JSONSchema> ProcessTool::getSchema() const {
  return shared::zObject({
      {"action",
       shared::zEnum({"Execute", "Spawn", "Status", "Wait", "Input", "Output", "List", "Kill"})
           ->describe(
               "Which process operation to perform.\n\n"
               "- Execute: run a command to completion\n"
               "- Spawn: start a background process\n"
               "- Status: inspect a process\n"
               "- Wait: wait for completion and/or output conditions\n"
               "- Input: send stdin to a live process\n"
               "- Output: read buffered stdout/stderr\n"
               "- List: list managed processes\n"
               "- Kill: terminate a managed process")},
      {"command",
       shared::zString()->setOptional()->describe(
           "Shell command to run for Execute or Spawn.\n\n"
           "Required for Execute/Spawn. Use workspace-safe verification commands when possible.")},
      {"process_id",
       shared::zString()->setOptional()->describe(
           "Managed process identifier returned by Spawn.\n\n"
           "Required for Status/Wait/Input/Output/Kill.")},
      {"input",
       shared::zString()->setOptional()->describe(
           "Text to send to stdin for Input action.")},
      {"pattern",
       shared::zString()->setOptional()->describe(
           "Optional regex/text pattern used by Wait to stop when matching output appears.")},
      {"stream",
       shared::zString()->setOptional()->describe(
           "Which output stream to inspect for Output action. Typically 'stdout' or 'stderr'.")},
      {"cwd",
       shared::zString()->setOptional()->describe(
           "Working directory for Execute/Spawn. Prefer workspace-relative directories.")},
      {"timeout_ms",
       shared::zInteger()->setOptional()->describe(
           "Timeout in milliseconds for Execute/Wait. Use to bound verification steps.")},
      {"from_offset",
       shared::zInteger()->setOptional()->describe(
           "Byte offset for Output reads so you can continue from a prior cursor.")},
      {"max_bytes",
       shared::zInteger()->setOptional()->describe(
           "Maximum bytes to return for Output. Use to keep large logs bounded.")},
      {"tail_lines",
       shared::zInteger()->setOptional()->describe(
           "Optional number of trailing lines to return for Output/List-style inspection.")},
  });
}

shared::ToolResult ProcessTool::execute(const rapidjson::Value &input, shared::ToolContext &ctx) {
  if (!input.IsObject()) {
    return shared::ToolResult::fail("Process input must be an object");
  }
  std::string action;
  if (input.HasMember("action") && input["action"].IsString()) {
    action = input["action"].GetString();
  } else if (input.HasMember("command") && input["command"].IsString()) {
    action = "Spawn";
  } else if (input.HasMember("input") && input["input"].IsString()) {
    action = "Input";
  } else if (input.HasMember("process_id") && input["process_id"].IsString() &&
             (input.HasMember("pattern") || input.HasMember("timeout_ms"))) {
    action = "Wait";
  } else if (input.HasMember("process_id") && input["process_id"].IsString()) {
    action = "Status";
  } else {
    return shared::ToolResult::fail("Process.action must be a string (Execute, Spawn, Status, Wait, Input, Output, List, or Kill)");
  }
  if (action == "Execute") return executeExecute(input, ctx);
  if (action == "Spawn") return executeSpawn(input, ctx);
  if (action == "Status") return executeStatus(input, ctx);
  if (action == "Wait") return executeWait(input, ctx);
  if (action == "Input") return executeInput(input, ctx);
  if (action == "Output") return executeOutput(input, ctx);
  if (action == "List") return executeList(input, ctx);
  if (action == "Kill") return executeKill(input, ctx);
  return shared::ToolResult::fail("Process.action must be Execute, Spawn, Status, Wait, Input, Output, List, or Kill");
}

} // namespace firmius::core

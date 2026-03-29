#include "tools/ProcessExecuteTool.hpp"
#include "IAgent.hpp"
#include "agents/AgentPermissionChecks.hpp"
#include "utils/FSUtil.hpp"
#include <chrono>
#include <cctype>
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

} // namespace

shared::ToolMetadata ProcessExecuteTool::getMetadata() const {
  return {"process_execute", "Execute a command on the host",
          ToolScope::Process};
}

std::shared_ptr<shared::JSONSchema> ProcessExecuteTool::getSchema() const {
  return zObject({{"command", zString()->describe("Shell command to execute")},
                  {"cwd", zString()
                              ->describe("Working directory for the command")
                              ->setOptional()},
                  {"timeout_ms",
                   zInteger()
                       ->describe("Timeout in milliseconds (default 15000)")
                       ->setOptional()}})
      ->required({"command"});
}

shared::ToolResult ProcessExecuteTool::execute(const ProcessExecuteInput &input,
                                               shared::ToolContext &ctx) {
  std::string processId;
  shared::ProcessSnapshot snap;
  try {
    std::string normalizedCommand;
    normalizedCommand.reserve(input.command.size());
    bool previousWasSpace = false;
    for (char ch : input.command) {
      if (std::isspace(static_cast<unsigned char>(ch))) {
        if (!previousWasSpace) {
          normalizedCommand.push_back(' ');
          previousWasSpace = true;
        }
      } else {
        normalizedCommand.push_back(
            static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
        previousWasSpace = false;
      }
    }
    if (!normalizedCommand.empty() && normalizedCommand.front() == ' ') {
      normalizedCommand.erase(normalizedCommand.begin());
    }
    if (!normalizedCommand.empty() && normalizedCommand.back() == ' ') {
      normalizedCommand.pop_back();
    }

    const bool mentionsApplyPatch =
        normalizedCommand == "apply_patch" ||
        normalizedCommand.rfind("apply_patch ", 0) == 0 ||
        normalizedCommand.find(" apply_patch ") != std::string::npos ||
        normalizedCommand.find("| apply_patch") != std::string::npos ||
        normalizedCommand.find("&& apply_patch") != std::string::npos ||
        normalizedCommand.find("|| apply_patch") != std::string::npos;
    if (mentionsApplyPatch) {
      return shared::ToolResult::fail(
          "Foreign tool conflict: 'apply_patch' is not available in Firmius "
          "and must not be run via process_execute. Use file_read + file_edit "
          "instead.");
    }

    std::string effectiveCwd =
        input.cwd.empty() ? ctx.agent.getContext().environment.cwd : input.cwd;
    // Normalize path first
    effectiveCwd = ctx.agent.getEnvironment()->getWorkspace().resolvePath(effectiveCwd);

    ctx.agent.getPermissions()->validatePathAccess(effectiveCwd, firmius::shared::AccessMode::READ);
    auto intent = ctx.agent.getPermissions()->getIntentAnalyzer().analyze(
        input.command, effectiveCwd);
    auto approval =
        ctx.agent.getPermissions()->requestCommandApproval(input.command, intent);
    if (approval == PermissionResponse::Deny) {
      return shared::ToolResult::fail("Command execution denied: " +
                                      input.command);
    }

    processId = ctx.agent.getEnvironment()->getProcessManager().spawnProcess(
        input.command, ctx.currentToolCallId, effectiveCwd, {}, true);
    ctx.agent.getEnvironment()->getProcessManager().addBlockingProcessId(processId);

    auto timeoutMs = (input.timeout_ms > 0) ? input.timeout_ms : 15000;
    auto start = std::chrono::steady_clock::now();

    auto sleepDuration = std::chrono::milliseconds(1);
    const auto maxSleep = std::chrono::milliseconds(20);

    while (true) {
      // Check for interrupt at the top of each iteration
      if (ctx.cancelRequested()) {
        // Get latest snapshot FIRST to capture all output before killing
        snap = ctx.agent.getEnvironment()->getProcessManager().inspectProcess(processId);
        // Kill the process to ensure cleanup
        ctx.agent.getEnvironment()->getProcessManager().killProcess(processId);
        ctx.agent.getEnvironment()->getProcessManager().removeBlockingProcessId(processId);

        rapidjson::Document doc;
        doc.SetObject();
        auto &a = doc.GetAllocator();
        doc.AddMember("exit_code", -2, a);
        doc.AddMember("stdout",
                      rapidjson::Value(snap.stdoutData.c_str(), a).Move(), a);
        doc.AddMember("stderr",
                      rapidjson::Value(snap.stderrData.c_str(), a).Move(), a);
        doc.AddMember("duration_ms", snap.elapsedMs, a);
        doc.AddMember("finish_reason", "Interrupted", a);
        doc.AddMember("command_success", false, a);
        doc.AddMember("process_id",
                      rapidjson::Value(processId.c_str(), a).Move(), a);
        return shared::ToolResult::ok(doc, processId);
      }

      snap = ctx.agent.getEnvironment()->getProcessManager().inspectProcess(processId);
      if (!snap.running)
        break;

      auto now = std::chrono::steady_clock::now();
      auto elapsed =
          std::chrono::duration_cast<std::chrono::milliseconds>(now - start)
              .count();
      if (elapsed > timeoutMs) {
        ctx.agent.getEnvironment()->getProcessManager().removeBlockingProcessId(processId);

        rapidjson::Document doc;
        doc.SetObject();
        auto &a = doc.GetAllocator();
        doc.AddMember("exit_code", -1, a);
        doc.AddMember("stdout",
                      rapidjson::Value(snap.stdoutData.c_str(), a).Move(), a);
        doc.AddMember("stderr",
                      rapidjson::Value(snap.stderrData.c_str(), a).Move(), a);
        doc.AddMember("duration_ms", snap.elapsedMs, a);
        doc.AddMember("finish_reason", "Timeout", a);
        doc.AddMember("command_success", false, a);
        doc.AddMember("process_id",
                      rapidjson::Value(processId.c_str(), a).Move(), a);
        doc.AddMember("note",
                      rapidjson::Value("The command is still running in the background. Use "
                                       "process_status to check its current output or "
                                       "process_wait to wait for its completion.", a).Move(), a);

        auto result = shared::ToolResult::ok(doc, processId);
        result.is_background = true;
        return result;
      }

      if (!ctx.waitFor(sleepDuration)) {
        ctx.agent.getEnvironment()->getProcessManager().removeBlockingProcessId(processId);
        rapidjson::Document doc;
        doc.SetObject();
        auto &a = doc.GetAllocator();
        doc.AddMember("exit_code", -2, a);
        doc.AddMember("stdout",
                      rapidjson::Value(snap.stdoutData.c_str(), a).Move(), a);
        doc.AddMember("stderr",
                      rapidjson::Value(snap.stderrData.c_str(), a).Move(), a);
        doc.AddMember("duration_ms", snap.elapsedMs, a);
        doc.AddMember("finish_reason", "Interrupted", a);
        doc.AddMember("command_success", false, a);
        doc.AddMember("process_id",
                      rapidjson::Value(processId.c_str(), a).Move(), a);
        return shared::ToolResult::ok(doc, processId);
      }
      if (sleepDuration < maxSleep) {
        sleepDuration = std::min(maxSleep, sleepDuration * 2);
      }
    }

    ctx.agent.getEnvironment()->getProcessManager().removeBlockingProcessId(processId);

    rapidjson::Document doc;
    doc.SetObject();
    auto &a = doc.GetAllocator();
    doc.AddMember("exit_code", snap.exitCode, a);
    doc.AddMember("stdout", rapidjson::Value(snap.stdoutData.c_str(), a).Move(),
                  a);
    doc.AddMember("stderr", rapidjson::Value(snap.stderrData.c_str(), a).Move(),
                  a);
    doc.AddMember("duration_ms", snap.elapsedMs, a);
    doc.AddMember("finish_reason", "Natural", a);
    doc.AddMember("command_success", snap.exitCode == 0, a);
    doc.AddMember("process_id",
                  rapidjson::Value(processId.c_str(), a).Move(), a);

    if (snap.exitCode != 0) {
      return failWithStructuredData(
          doc, "Command exited with non-zero exit code: " +
                   std::to_string(snap.exitCode));
    }

    return shared::ToolResult::ok(doc, processId);
  } catch (const std::exception &e) {
    if (!processId.empty())
      ctx.agent.getEnvironment()->getProcessManager().removeBlockingProcessId(processId);
    return shared::ToolResult::fail(e.what());
  }
}

} // namespace firmius::core

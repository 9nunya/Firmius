#include "tools/PythonExecuteTool.hpp"
#include "agents/Agent.hpp"
#include "agents/AgentPermissionChecks.hpp"
#include "utils/FSUtil.hpp"
#include "utils/StringUtil.hpp"
#include <filesystem>
#include <map>
#include <rapidjson/document.h>
#include <thread>

namespace firmius::core {
using namespace firmius::shared;

shared::ToolMetadata PythonExecuteTool::getMetadata() const {
  return {"python_execute", "Executes arbitrary Python code on the host.",
          shared::ToolScope::Process};
}

std::shared_ptr<shared::JSONSchema> PythonExecuteTool::getSchema() const {
  return shared::zObject({{"code", shared::zString()->describe(
                                       "The Python code to execute.")}})
      ->required({"code"});
}

shared::ToolResult PythonExecuteTool::execute(const PythonExecuteInput &input,
                                              shared::ToolContext &ctx) {
  static const char *kCodeEnvVar = "FIRMIUS_PYTHON_EXECUTE_CODE";
  std::string command =
      "python3 -c \"import os; exec(compile(os.environ['" +
      std::string(kCodeEnvVar) + "'], '<python_execute>', 'exec'))\"";
  std::string processId;

  try {
    std::string effectiveCwd = ctx.agent.getContext().environment.cwd;
    effectiveCwd = ctx.agent.getEnvironment()->getWorkspace().resolvePath(effectiveCwd);

    ctx.agent.getPermissions()->validatePathAccess(
        effectiveCwd, firmius::shared::AccessMode::READ);
    auto intent =
        ctx.agent.getPermissions()->getIntentAnalyzer().analyze(command, effectiveCwd);
    auto approval =
        ctx.agent.getPermissions()->requestCommandApproval(command, intent);
    if (approval == PermissionResponse::Deny) {
      return shared::ToolResult::fail("Command execution denied: " + command);
    }

    std::map<std::string, std::string> env = {
        {kCodeEnvVar, input.code},
        {"FIRMIUS_PYTHON_EXECUTE_LINES",
         std::to_string(std::count(input.code.begin(), input.code.end(), '\n') +
                        (!input.code.empty() ? 1 : 0))}};

    processId = ctx.agent.getEnvironment()->getProcessManager().spawnProcess(
        command, ctx.currentToolCallId, effectiveCwd, env);
    ctx.agent.getEnvironment()->getProcessManager().addBlockingProcessId(processId);

    shared::ProcessSnapshot snap;
    auto sleepDuration = std::chrono::milliseconds(1);
    const auto maxSleep = std::chrono::milliseconds(100);

    while (true) {
      // Check for interrupt
      if (ctx.agent.isInterrupted()) {
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
        doc.AddMember("process_id",
                      rapidjson::Value(processId.c_str(), a).Move(), a);
        doc.AddMember("command",
                      rapidjson::Value(command.c_str(), a).Move(), a);
        return shared::ToolResult::ok(doc, processId);
      }

      snap = ctx.agent.getEnvironment()->getProcessManager().inspectProcess(processId);
      if (!snap.running)
        break;

      std::this_thread::sleep_for(sleepDuration);
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
    doc.AddMember("process_id",
                  rapidjson::Value(processId.c_str(), a).Move(), a);
    doc.AddMember("command",
                  rapidjson::Value(command.c_str(), a).Move(), a);

    return shared::ToolResult::ok(doc, processId);
  } catch (const std::exception &e) {
    if (!processId.empty())
      ctx.agent.getEnvironment()->getProcessManager().removeBlockingProcessId(processId);
    return shared::ToolResult::fail(e.what());
  }
}

} // namespace firmius::core

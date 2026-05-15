#include "tools/PythonExecuteTool.hpp"

#include "agents/Agent.hpp"
#include <chrono>
#include <filesystem>
#include <map>
#include <rapidjson/document.h>

namespace firmius::core {
using namespace firmius::shared;

shared::ToolMetadata PythonExecuteTool::getMetadata() const {
  return {"Python",
          R"(Execute Python code on the host as a real process.

USAGE GUIDANCE:
- Use this only when Python is the correct execution surface, not as a shortcut for editing files.
- Do NOT use Python to write repository files; use the Edit-family tools for all file modifications.
- Good uses: structured data inspection, one-off calculations, parsing/transforms for analysis, probing environments that already have the needed interpreter/packages.
- If a project virtualenv should be used, pass venv so the tool runs that interpreter.

EXECUTION MODEL:
- The tool runs Python via a managed process and returns stdout/stderr/exit_code.
- It is subject to normal command approval / process permissions.
)",
          shared::ToolScope::Process};
}

std::shared_ptr<shared::JSONSchema> PythonExecuteTool::getSchema() const {
  return shared::zObject(
             {{"code", shared::zString()->describe(
                            "Python source code to execute.\n\n"
                            "Use for analysis/execution, not file editing. The code is executed as a managed host process and its stdout/stderr are returned.")},
              {"venv", shared::zString()
                           ->describe(
                               "Optional path to a Python virtual environment whose interpreter should be used.\n\n"
                               "Pass the virtualenv root (the tool resolves '<venv>/bin/python'). The path must be readable under current permissions.")
                           ->setOptional()}})
      ->required({"code"});
}

shared::ToolResult PythonExecuteTool::execute(const PythonExecuteInput &input,
                                              shared::ToolContext &ctx) {
  static const char *kCodeEnvVar = "FIRMIUS_PYTHON_EXECUTE_CODE";

  std::string pythonExecutable = "python3";
  std::string resolvedVenv;
  std::string processId;

  try {
    if (!input.venv.empty()) {
      resolvedVenv =
          ctx.agent.getEnvironment()->getWorkspace().resolvePath(input.venv);
      ctx.agent.getPermissions()->validatePathAccess(
          resolvedVenv, firmius::shared::AccessMode::READ);
      pythonExecutable =
          (std::filesystem::path(resolvedVenv) / "bin" / "python").string();
    }

    std::string command =
        pythonExecutable +
        " -c \"import os; exec(compile(os.environ['" +
        std::string(kCodeEnvVar) +
        "'], '<python_execute>', 'exec'))\"";

    std::string effectiveCwd = ctx.agent.getContext().environment.cwd;
    effectiveCwd =
        ctx.agent.getEnvironment()->getWorkspace().resolvePath(effectiveCwd);

    ctx.agent.getPermissions()->validatePathAccess(
        effectiveCwd, firmius::shared::AccessMode::READ);
    auto intent = ctx.agent.getPermissions()->getIntentAnalyzer().analyze(
        command, effectiveCwd);
    auto approval =
        ctx.agent.getPermissions()->requestCommandApproval(command, intent, "Python");
    if (approval == PermissionResponse::Deny) {
      return shared::ToolResult::fail("Command execution denied: " + command);
    }

    std::map<std::string, std::string> env = {
        {kCodeEnvVar, input.code},
        {"FIRMIUS_PYTHON_EXECUTE_LINES",
         std::to_string(std::count(input.code.begin(), input.code.end(), '\n') +
                        (!input.code.empty() ? 1 : 0))}};
    if (!resolvedVenv.empty()) {
      env["VIRTUAL_ENV"] = resolvedVenv;
    }

    processId = ctx.agent.getEnvironment()->getProcessManager().spawnProcess(
        command, ctx.currentToolCallId, effectiveCwd, env);
    ctx.agent.getEnvironment()->getProcessManager().addBlockingProcessId(processId);

    shared::ProcessSnapshot snap;
    auto sleepDuration = std::chrono::milliseconds(1);
    const auto maxSleep = std::chrono::milliseconds(20);

    while (true) {
      if (ctx.cancelRequested()) {
        ctx.agent.getEnvironment()->getProcessManager().removeBlockingProcessId(
            processId);
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
        if (!resolvedVenv.empty()) {
          doc.AddMember("venv",
                        rapidjson::Value(resolvedVenv.c_str(), a).Move(), a);
        }
        return shared::ToolResult::ok(doc, processId);
      }

      snap =
          ctx.agent.getEnvironment()->getProcessManager().inspectProcess(processId);
      if (!snap.running) {
        break;
      }

      if (!ctx.waitFor(sleepDuration)) {
        ctx.agent.getEnvironment()->getProcessManager().removeBlockingProcessId(
            processId);
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
        if (!resolvedVenv.empty()) {
          doc.AddMember("venv",
                        rapidjson::Value(resolvedVenv.c_str(), a).Move(), a);
        }
        return shared::ToolResult::ok(doc, processId);
      }

      if (sleepDuration < maxSleep) {
        sleepDuration = std::min(maxSleep, sleepDuration * 2);
      }
    }

    ctx.agent.getEnvironment()->getProcessManager().removeBlockingProcessId(
        processId);

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
    doc.AddMember("process_id", rapidjson::Value(processId.c_str(), a).Move(),
                  a);
    doc.AddMember("command", rapidjson::Value(command.c_str(), a).Move(), a);
    if (!resolvedVenv.empty()) {
      doc.AddMember("venv", rapidjson::Value(resolvedVenv.c_str(), a).Move(), a);
    }

    return shared::ToolResult::ok(doc, processId);
  } catch (const std::exception &e) {
    if (!processId.empty()) {
      ctx.agent.getEnvironment()->getProcessManager().removeBlockingProcessId(
          processId);
    }
    return shared::ToolResult::fail(e.what());
  }
}

} // namespace firmius::core

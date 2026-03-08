#include "tools/ProcessExecuteTool.hpp"
#include "IAgent.hpp"
#include "utils/FSUtil.hpp"
#include <chrono>
#include <optional>
#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>
#include <thread>

namespace firmius::core {
using namespace firmius::shared;

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
    std::string effectiveCwd =
        input.cwd.empty() ? ctx.agent.getContext().environment.cwd : input.cwd;
    // Normalize path first
    effectiveCwd = ctx.agent.resolvePath(effectiveCwd);

    // Security check
    bool allowed = false;
    for (const auto& p : ctx.agent.getContext().permissions.allowedPaths) {
        if (shared::FSUtil::isSubpath(effectiveCwd, p)) {
            allowed = true;
            break;
        }
    }
    if (!allowed && !ctx.agent.getContext().permissions.allowOutsideCwd) {
        return shared::ToolResult::fail("Access denied: cwd outside allowed directories: " + effectiveCwd);
    }

    processId = ctx.agent.spawnProcess(input.command, ctx.currentToolCallId,
                                       effectiveCwd, {});
    ctx.agent.addBlockingProcessId(processId);

    auto timeoutMs = (input.timeout_ms > 0) ? input.timeout_ms : 15000;
    auto start = std::chrono::steady_clock::now();

    auto sleepDuration = std::chrono::milliseconds(1);
    const auto maxSleep = std::chrono::milliseconds(50);

    while (true) {
      // Check for interrupt at the top of each iteration
      if (ctx.agent.isInterrupted()) {
        ctx.agent.removeBlockingProcessId(processId);

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
        return shared::ToolResult::ok(doc, processId);
      }

      snap = ctx.agent.inspectProcess(processId);
      if (!snap.running)
        break;

      auto now = std::chrono::steady_clock::now();
      auto elapsed =
          std::chrono::duration_cast<std::chrono::milliseconds>(now - start)
               .count();
      if (elapsed > timeoutMs) {
        ctx.agent.removeBlockingProcessId(processId);

        rapidjson::Document doc;
        doc.SetObject();
        auto &a = doc.GetAllocator();
        doc.AddMember("exit_code", -1, a);
        doc.AddMember("stdout",
                      rapidjson::Value(snap.stdoutData.c_str(), a).Move(), a);
        doc.AddMember("stderr",
                      rapidjson::Value(snap.stderrData.c_str(), a).Move(), a);
        doc.AddMember("duration_ms", (double)elapsed, a);
        doc.AddMember("finish_reason", "Timeout", a);
        doc.AddMember("process_id",
                      rapidjson::Value(processId.c_str(), a).Move(), a);
        doc.AddMember("message",
                      "Process timed out but continues in background.", a);
        return shared::ToolResult::ok(doc, processId);
      }

      std::this_thread::sleep_for(sleepDuration);
      if (sleepDuration < maxSleep) {
          sleepDuration = std::min(maxSleep, sleepDuration * 2);
      }
    }

    ctx.agent.removeBlockingProcessId(processId);

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

    return shared::ToolResult::ok(doc, processId);
  } catch (const std::exception &e) {
    if (!processId.empty())
      ctx.agent.removeBlockingProcessId(processId);
    return shared::ToolResult::fail(e.what());
  }
}

} // namespace firmius::core

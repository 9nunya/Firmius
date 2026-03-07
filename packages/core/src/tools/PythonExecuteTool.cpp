#include "tools/PythonExecuteTool.hpp"
#include "IAgent.hpp"
#include "utils/StringUtil.hpp"
#include <filesystem>
#include <thread>
#include <rapidjson/document.h>

namespace firmius::core {
using namespace firmius::shared;

shared::ToolMetadata PythonExecuteTool::getMetadata() const {
    return {"python_execute", "Executes arbitrary Python code on the host.", shared::ToolScope::Process};
}

std::shared_ptr<shared::JSONSchema> PythonExecuteTool::getSchema() const {
    return shared::zObject({
        {"code", shared::zString()->describe("The Python code to execute.")}
    })->required({"code"});
}

shared::ToolResult PythonExecuteTool::execute(const PythonExecuteInput& input, shared::ToolContext& ctx) {
    std::string tempFile = "/tmp/firmius_script_" + shared::StringUtil::generateUuid() + ".py";
    std::string processId;

    try {
        ctx.host.writeFile(tempFile, std::vector<uint8_t>(input.code.begin(), input.code.end()));

        processId = ctx.agent.spawnProcess("python3 " + tempFile, ctx.currentToolCallId, ctx.agent.getContext().environment.cwd);
        ctx.agent.addBlockingProcessId(processId);

        shared::ProcessSnapshot snap;
        auto sleepDuration = std::chrono::milliseconds(1);
        const auto maxSleep = std::chrono::milliseconds(100);

        while (true) {
            // Check for interrupt
            if (ctx.agent.isInterrupted()) {
                ctx.agent.removeBlockingProcessId(processId);
                rapidjson::Document doc;
                doc.SetObject();
                auto& a = doc.GetAllocator();
                doc.AddMember("exit_code", -2, a);
                doc.AddMember("output", rapidjson::Value(snap.stdoutData.c_str(), a).Move(), a);
                doc.AddMember("duration_ms", snap.elapsedMs, a);
                doc.AddMember("finish_reason", "Interrupted", a);
                doc.AddMember("process_id", rapidjson::Value(processId.c_str(), a).Move(), a);
                return shared::ToolResult::ok(doc);
            }

            snap = ctx.agent.inspectProcess(processId);
            if (!snap.running) break;

            std::this_thread::sleep_for(sleepDuration);
            if (sleepDuration < maxSleep) {
                sleepDuration = std::min(maxSleep, sleepDuration * 2);
            }
        }

        ctx.agent.removeBlockingProcessId(processId);

        // Cleanup
        ctx.host.exec("rm " + tempFile);

        rapidjson::Document doc;
        doc.SetObject();
        auto& a = doc.GetAllocator();
        doc.AddMember("exit_code", snap.exitCode, a);
        doc.AddMember("stdout", rapidjson::Value(snap.stdoutData.c_str(), a).Move(), a);
        doc.AddMember("stderr", rapidjson::Value(snap.stderrData.c_str(), a).Move(), a);
        doc.AddMember("duration_ms", snap.elapsedMs, a);

        return shared::ToolResult::ok(doc);
    } catch (const std::exception& e) {
        if (!processId.empty()) ctx.agent.removeBlockingProcessId(processId);
        return shared::ToolResult::fail(e.what());
    }
}

}

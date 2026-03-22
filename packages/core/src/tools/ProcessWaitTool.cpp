#include "tools/ProcessWaitTool.hpp"
#include "IAgent.hpp"
#include <rapidjson/document.h>
#include <thread>
#include <chrono>

namespace firmius::core {

shared::ToolResult ProcessWaitTool::execute(const ProcessWaitInput& input, shared::ToolContext& ctx) {
    try {
        auto startTime = std::chrono::steady_clock::now();
        int timeout = input.timeout_ms > 0 ? input.timeout_ms : 30000;

        while (true) {
            if (ctx.cancelRequested()) {
                return shared::ToolResult::fail("Interrupted");
            }
            auto snapshot = ctx.agent.getEnvironment()->getProcessManager().inspectProcess(input.process_id);

            bool patternFound = false;
            if (!input.pattern.empty()) {
                if (snapshot.stdoutData.find(input.pattern) != std::string::npos ||
                    snapshot.stderrData.find(input.pattern) != std::string::npos) {
                    patternFound = true;
                }
            }

            if (!snapshot.running || patternFound) {
                rapidjson::Document doc;
                doc.SetObject();
                auto& a = doc.GetAllocator();
                doc.AddMember("isRunning", snapshot.running, a);
                doc.AddMember("exitCode", snapshot.exitCode, a);
                doc.AddMember("stdout", rapidjson::Value(snapshot.stdoutData.c_str(), a).Move(), a);
                doc.AddMember("stderr", rapidjson::Value(snapshot.stderrData.c_str(), a).Move(), a);
                doc.AddMember("patternFound", patternFound, a);
                return shared::ToolResult::ok(doc);
            }

            auto now = std::chrono::steady_clock::now();
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - startTime).count();
            if (elapsed >= timeout) {
                return shared::ToolResult::fail("Timeout reached while waiting for process " + input.process_id);
            }

            if (!ctx.waitFor(std::chrono::milliseconds(25))) {
                return shared::ToolResult::fail("Interrupted");
            }
        }
    } catch (const std::exception& e) {
        return shared::ToolResult::fail(e.what());
    }
}

}

#include "tools/ProcessStatusTool.hpp"
#include "IAgent.hpp"
#include <rapidjson/document.h>

namespace firmius::core {

shared::ToolResult ProcessStatusTool::execute(const ProcessStatusInput& input, shared::ToolContext& ctx) {
    try {
        auto snapshot = ctx.agent.getEnvironment()->getProcessManager().inspectProcess(input.process_id);
        
        rapidjson::Document doc;
        doc.SetObject();
        auto& a = doc.GetAllocator();
        
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

}

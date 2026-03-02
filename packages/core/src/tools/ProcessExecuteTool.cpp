#include "tools/ProcessExecuteTool.hpp"
#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>
#include <chrono>
#include <optional>

namespace firmius::core {
using namespace firmius::shared;

shared::ToolMetadata ProcessExecuteTool::getMetadata() const {
    return {"process_execute", "Execute a command on the host", ToolScope::Process};
}

std::shared_ptr<shared::JSONSchema> ProcessExecuteTool::getSchema() const {
    return zObject({
        {"command", zString()->describe("Shell command to execute")},
        {"cwd", zString()->describe("Working directory for the command")->setOptional()},
        {"timeout_ms", zInteger()->describe("Timeout in milliseconds (default 15000)")->setOptional()}
    })->required({"command"});
}

shared::ToolResult ProcessExecuteTool::execute(const ProcessExecuteInput& input, shared::ToolContext& ctx) {
    try {
        auto timeoutOpt = (input.timeout_ms > 0) ? 
            std::optional<std::chrono::milliseconds>(std::chrono::milliseconds(input.timeout_ms)) : 
            std::nullopt;
        auto res = ctx.host.exec(input.command, input.cwd, {}, timeoutOpt);
        
        rapidjson::Document doc;
        doc.SetObject();
        auto& a = doc.GetAllocator();
        doc.AddMember("exit_code", res.exitCode, a);
        doc.AddMember("stdout", rapidjson::Value(res.stdoutData.c_str(), a).Move(), a);
        doc.AddMember("stderr", rapidjson::Value(res.stderrData.c_str(), a).Move(), a);
        doc.AddMember("duration_ms", res.durationMs, a);
        
        std::string reasonStr;
        switch (res.finishReason) {
            case ProcessFinishReason::Natural: reasonStr = "Natural"; break;
            case ProcessFinishReason::Timeout: reasonStr = "Timeout"; break;
            case ProcessFinishReason::Terminated: reasonStr = "Terminated"; break;
        }
        doc.AddMember("finish_reason", rapidjson::Value(reasonStr.c_str(), a).Move(), a);
        
        if (!res.backgroundProcessId.empty()) {
            doc.AddMember("process_id", rapidjson::Value(res.backgroundProcessId.c_str(), a).Move(), a);
            std::string msg = "Process timed out after " + std::to_string(static_cast<int>(res.durationMs)) + " ms and continues running in background. Use process_input, process_status, and process_wait to interact with it.";
            doc.AddMember("message", rapidjson::Value(msg.c_str(), a).Move(), a);
        }
        
        return shared::ToolResult::ok(doc);
    } catch (const std::exception& e) {
        return shared::ToolResult::fail(e.what());
    }
}

}

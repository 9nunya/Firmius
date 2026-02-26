#include "tools/ProcessExecuteTool.hpp"
#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

namespace firmius::core {
using namespace firmius::shared;

shared::ToolMetadata ProcessExecuteTool::getMetadata() const {
    return {"process_execute", "Execute a command on the host", ToolScope::Process};
}

std::shared_ptr<shared::JSONSchema> ProcessExecuteTool::getSchema() const {
    return zObject({
        {"command", zString()->describe("Shell command to execute")},
        {"cwd", zString()->describe("Working directory for the command")->setOptional()}
    })->required({"command"});
}

shared::ToolResult ProcessExecuteTool::execute(const ProcessExecuteInput& input, shared::ToolContext& ctx) {
    try {
        auto res = ctx.host.exec(input.command, input.cwd);
        
        rapidjson::Document doc;
        doc.SetObject();
        auto& a = doc.GetAllocator();
        doc.AddMember("exit_code", res.exitCode, a);
        doc.AddMember("stdout", rapidjson::Value(res.stdoutData.c_str(), a).Move(), a);
        doc.AddMember("stderr", rapidjson::Value(res.stderrData.c_str(), a).Move(), a);
        doc.AddMember("duration_ms", res.durationMs, a);
        
        return shared::ToolResult::ok(std::move(doc));
    } catch (const std::exception& e) {
        return shared::ToolResult::fail(e.what());
    }
}

}

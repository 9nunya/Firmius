#include "tools/PythonExecuteTool.hpp"
#include "IAgent.hpp"
#include "utils/StringUtil.hpp"
#include <filesystem>

namespace firmius::core {

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
    
    try {
        ctx.host.writeFile(tempFile, std::vector<uint8_t>(input.code.begin(), input.code.end()));
        auto res = ctx.host.exec("python3 " + tempFile, ctx.agent.getContext().environment.cwd);
        
        // Cleanup
        ctx.host.exec("rm " + tempFile);

        rapidjson::Document doc;
        doc.SetObject();
        auto& a = doc.GetAllocator();
        doc.AddMember("exit_code", res.exitCode, a);
        doc.AddMember("stdout", rapidjson::Value(res.stdoutData.c_str(), a).Move(), a);
        doc.AddMember("stderr", rapidjson::Value(res.stderrData.c_str(), a).Move(), a);
        doc.AddMember("duration_ms", res.durationMs, a);
        
        return shared::ToolResult::ok(doc);
    } catch (const std::exception& e) {
        return shared::ToolResult::fail(e.what());
    }
}

}

#include "tools/SubagentWaitTool.hpp"
#include "Engine.hpp"

namespace firmius::core {

shared::ToolMetadata SubagentWaitTool::getMetadata() const {
    return {"subagent_wait", "Wait for a subagent to complete and return its result.", shared::ToolScope::Delegation};
}

std::shared_ptr<shared::JSONSchema> SubagentWaitTool::getSchema() const {
    return shared::zObject({
        {"agent_id", shared::zString()->describe("The unique ID of the agent to wait for.")}
    })->required({"agent_id"});
}

shared::ToolResult SubagentWaitTool::execute(const SubagentWaitInput& input, shared::ToolContext& ctx) {
    std::string result;
    while (true) {
        auto res = Engine::instance().waitForAgent(input.agent_id, std::chrono::milliseconds(100));
        if (res.has_value()) {
            result = *res;
            break;
        }
        if (ctx.agent.isInterrupted()) {
            Engine::instance().cancelAgent(input.agent_id);
            return shared::ToolResult::fail("Parent agent interrupted while waiting for subagent.");
        }
    }
    
    rapidjson::Document d;
    d.SetObject();
    auto& a = d.GetAllocator();
    d.AddMember("agentId", rapidjson::Value(input.agent_id.c_str(), a).Move(), a);
    d.AddMember("result", rapidjson::Value(result.c_str(), a).Move(), a);
    
    if (result.find("Error:") == 0) {
        return shared::ToolResult::fail(result);
    }
    
    return shared::ToolResult::ok(d);
}

}

#include "tools/SubagentTerminateTool.hpp"
#include "Engine.hpp"
#include <rapidjson/document.h>

namespace firmius::core {

shared::ToolMetadata SubagentTerminateTool::getMetadata() const {
    return {"terminate_subagent", "Explicitly destroy a subagent and its environment.", shared::ToolScope::Delegation};
}

std::shared_ptr<shared::JSONSchema> SubagentTerminateTool::getSchema() const {
    return shared::zObject({
        {"agent_id", shared::zString()->describe("ID of the subagent to terminate")}
    })->required({"agent_id"});
}

shared::ToolResult SubagentTerminateTool::execute(const SubagentTerminateInput& input, shared::ToolContext& ctx) {
    (void)ctx; // Unused
    Engine::instance().terminateAgent(input.agent_id);

    rapidjson::Document d;
    d.SetObject();
    auto& a = d.GetAllocator();
    d.AddMember("agent_id", rapidjson::Value(input.agent_id.c_str(), a).Move(), a);
    d.AddMember("status", "terminated", a);
    return shared::ToolResult::ok(d);
}

}

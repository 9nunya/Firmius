#include "tools/SubagentTool.hpp"
#include "Engine.hpp"
#include "AgentRegistry.hpp"
#include <future>
#include <mutex>
#include <condition_variable>

namespace firmius::core {

shared::ToolMetadata SubagentTool::getMetadata() const {
    return {"summon_subagent", "Summon a child agent to perform a sub-task.", shared::ToolScope::Delegation};
}

std::shared_ptr<shared::JSONSchema> SubagentTool::getSchema() const {
    return shared::zObject({
        {"persona", shared::zString()->describe("Persona name (e.g., 'researcher')")},
        {"task", shared::zString()->describe("Description of the task")},
        {"async", shared::zBoolean()->describe("If true, returns immediately with agent_id")->setOptional()}
    })->required({"persona", "task"});
}

shared::ToolResult SubagentTool::execute(const SubagentInput& input, shared::ToolContext& ctx) {
    std::string threadId = ctx.agent.getContext().history.threadId;
    
    if (input.async) {
        std::string subagentId = Engine::instance().summonAgent(threadId, input.persona, input.task);
        rapidjson::Document d;
        d.SetObject();
        auto& a = d.GetAllocator();
        d.AddMember("agentId", rapidjson::Value(subagentId.c_str(), a).Move(), a);
        d.AddMember("status", "spawned", a);
        return shared::ToolResult::ok(d);
    } else {
        std::string subagentId = Engine::instance().summonAgent(threadId, input.persona, input.task);
        
        // Blocking wait using the Engine's future-based mechanism
        std::string resultSummary = Engine::instance().waitForAgent(subagentId);

        rapidjson::Document d;
        d.SetObject();
        auto& a = d.GetAllocator();
        d.AddMember("agentId", rapidjson::Value(subagentId.c_str(), a).Move(), a);
        d.AddMember("status", "completed", a);
        d.AddMember("result", rapidjson::Value(resultSummary.c_str(), a).Move(), a);
        return shared::ToolResult::ok(d);
    }
}

}

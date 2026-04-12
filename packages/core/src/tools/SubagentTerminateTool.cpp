#include "tools/SubagentTerminateTool.hpp"
#include "Engine.hpp"
#include "tools/WorkToolCommon.hpp"
#include <rapidjson/document.h>

namespace firmius::core {

namespace {

std::size_t releaseOwnedChunksForAgent(const std::string &threadId,
                                       const std::string &agentId) {
    ThreadManager tm(ThreadManager::defaultBasePath());
    std::size_t released = 0;

    for (auto plan : tm.listPlans(threadId)) {
        bool changed = false;
        for (auto &chunk : plan.chunks) {
            if (chunk.assignedAgentId != agentId) {
                continue;
            }

            const auto originalChunk = chunk;
            chunk.assignedAgentId.clear();
            if (chunk.status == shared::WorkChunkStatus::InProgress) {
                chunk.status = shared::WorkChunkStatus::Ready;
            }
            chunk.updatedAt = worktools::nowEpochMs();
            changed = true;
            ++released;

            worktools::emitWorkEvent(
                shared::ChunkUpdated{threadId, plan.id, chunk});
            worktools::emitWorkEvent(shared::ChunkAssigned{
                threadId, plan.id, chunk.id, chunk.assignedAgentId, chunk});
            if (originalChunk.status != chunk.status) {
                worktools::emitWorkEvent(shared::ChunkStatusChanged{
                    threadId, plan.id, chunk.id, originalChunk.status,
                    chunk.status, chunk});
            }
        }

        if (changed) {
            tm.updatePlan(threadId, plan);
        }
    }

    return released;
}

} // namespace

shared::ToolMetadata SubagentTerminateTool::getMetadata() const {
    return {"terminate_subagent", "Explicitly destroy a subagent and its environment.", shared::ToolScope::Delegation};
}

std::shared_ptr<shared::JSONSchema> SubagentTerminateTool::getSchema() const {
    return shared::zObject({
        {"agent_id", shared::zString()->describe("ID of the subagent to terminate")}
    })->required({"agent_id"});
}

shared::ToolResult SubagentTerminateTool::execute(const SubagentTerminateInput& input, shared::ToolContext& ctx) {
    const std::string threadId = worktools::requireCurrentThreadId(ctx);
    Engine::instance().terminateAgent(input.agent_id);
    const std::size_t releasedChunks =
        releaseOwnedChunksForAgent(threadId, input.agent_id);

    rapidjson::Document d;
    d.SetObject();
    auto& a = d.GetAllocator();
    d.AddMember("agent_id", rapidjson::Value(input.agent_id.c_str(), a).Move(), a);
    d.AddMember("status", "terminated", a);
    d.AddMember("released_chunk_count", static_cast<uint64_t>(releasedChunks), a);
    return shared::ToolResult::ok(d);
}

}

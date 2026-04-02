#include "tools/SubagentWaitTool.hpp"
#include "AgentRegistry.hpp"
#include "Engine.hpp"
#include "Serialization.hpp"
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>
namespace firmius::core {

namespace {

void appendOutcomeArtifacts(rapidjson::Document &d,
                            const shared::AgentOutcome &outcome) {
    auto &a = d.GetAllocator();
    auto appendArray = [&](const char *key,
                           const std::vector<shared::ThreadArtifactMetadata> &items) {
        rapidjson::Value array(rapidjson::kArrayType);
        for (const auto &artifact : items) {
            rapidjson::Document artifactDoc = shared::toJson(artifact);
            rapidjson::Value artifactValue;
            artifactValue.CopyFrom(artifactDoc, a);
            const std::string owner =
                artifact.ownerFriendlyName.empty() ? artifact.ownerAgentId
                                                   : artifact.ownerFriendlyName;
            const std::string reference =
                "@artifact:" + owner + "/" + artifact.filename;
            artifactValue.AddMember("reference",
                                    rapidjson::Value(reference.c_str(), a).Move(), a);
            array.PushBack(artifactValue, a);
        }
        d.AddMember(rapidjson::Value(key, a).Move(), array, a);
    };

    appendArray("artifacts_created", outcome.artifacts_created);
    appendArray("artifacts_updated", outcome.artifacts_updated);
}

shared::ToolResult failWithStructuredData(const rapidjson::Document &d,
                                          const std::string &error) {
    shared::ToolResult result = shared::ToolResult::fail(error);
    rapidjson::StringBuffer buffer;
    rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
    d.Accept(writer);
    result.data = buffer.GetString();
    return result;
}

} // namespace

shared::ToolMetadata SubagentWaitTool::getMetadata() const {
    return {"subagent_wait", "Wait for a subagent to complete and return its result.", shared::ToolScope::Delegation};
}

std::shared_ptr<shared::JSONSchema> SubagentWaitTool::getSchema() const {
    return shared::zObject({
        {"agent_id", shared::zString()->describe("The unique ID of the agent to wait for.")}
    })->required({"agent_id"});
}

shared::ToolResult SubagentWaitTool::execute(const SubagentWaitInput& input, shared::ToolContext& ctx) {
    std::optional<AgentOutcome> outcome;
    while (true) {
        outcome = Engine::instance().waitForAgentOutcome(input.agent_id, std::chrono::milliseconds(25));
        if (outcome.has_value()) {
            break;
        }
        if (ctx.cancelRequested()) {
            Engine::instance().cancelAgent(input.agent_id);
            return shared::ToolResult::fail("Parent agent interrupted while waiting for subagent.");
        }
    }
    
    rapidjson::Document d;
    d.SetObject();
    auto& a = d.GetAllocator();
    d.AddMember("agentId", rapidjson::Value(input.agent_id.c_str(), a).Move(), a);
    // Resolve friendlyName from AgentRegistry
    {
      auto agent = AgentRegistry::instance().getAgent(input.agent_id);
      const std::string friendlyName =
          agent && !agent->getContext().identity.friendlyName.empty()
              ? agent->getContext().identity.friendlyName
              : "";
      d.AddMember("friendlyName",
                  rapidjson::Value(friendlyName.c_str(), a).Move(), a);
    }
    appendOutcomeArtifacts(d, *outcome);
    if (outcome->kind == AgentOutcome::Kind::Cancelled) {
        d.AddMember("status", "cancelled", a);
        d.AddMember("result", rapidjson::Value(outcome->text.c_str(), a).Move(), a);
        return shared::ToolResult::ok(d);
    }
    if (outcome->kind == AgentOutcome::Kind::Failed) {
        d.AddMember("status", "failed", a);
        d.AddMember("error", rapidjson::Value(outcome->text.c_str(), a).Move(), a);
        return failWithStructuredData(d, outcome->text);
    }
    if (outcome->kind == AgentOutcome::Kind::NoSummary) {
        d.AddMember("status", "completed_no_summary", a);
        d.AddMember("result", rapidjson::Value(outcome->text.c_str(), a).Move(), a);
        return shared::ToolResult::ok(d);
    }

    d.AddMember("status", "completed", a);
    d.AddMember("result", rapidjson::Value(outcome->text.c_str(), a).Move(), a);
    return shared::ToolResult::ok(d);
}

}

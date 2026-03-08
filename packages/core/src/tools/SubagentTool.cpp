#include "tools/SubagentTool.hpp"
#include "AgentRegistry.hpp"
#include "Engine.hpp"
#include "Events.hpp"
#include <condition_variable>
#include <future>
#include <mutex>

namespace firmius::core {

shared::ToolMetadata SubagentTool::getMetadata() const {
  return {"summon_subagent", "Summon a child agent to perform a sub-task.",
          shared::ToolScope::Delegation};
}

std::shared_ptr<shared::JSONSchema> SubagentTool::getSchema() const {
  return shared::zObject(
             {{"persona", shared::zString()->describe(
                              "Persona name (e.g., 'researcher')")},
              {"task", shared::zString()->describe("Description of the task")},
              {"async",
               shared::zBoolean()
                   ->describe("If true, returns immediately with agent_id")
                   ->setOptional()},
              {"agent_id",
               shared::zString()
                   ->describe(
                       "ID of existing agent to re-task (omit to create new)")
                   ->setOptional()},
              {"name", shared::zString()->describe(
                           "Machine-friendly slug (e.g., 'auth-finder')")},
              {"title",
               shared::zString()->describe("Human-readable display name (e.g., "
                                           "'Find auth patterns')")}})
      ->required({"persona", "task", "name", "title"});
}

shared::ToolResult SubagentTool::execute(const SubagentInput &input,
                                         shared::ToolContext &ctx) {
  std::string threadId = ctx.agent.getContext().history->threadId;

  auto existingAgents = AgentRegistry::instance().listAll();
  for (const auto &agentId : existingAgents) {
    auto agent = AgentRegistry::instance().getAgent(agentId);
    if (agent && agent->getContext().identity.friendlyName == input.name) {
      return shared::ToolResult::fail("Agent name '" + input.name +
                                      "' already exists in this thread");
    }
  }

  // Check if re-tasking an existing agent
  if (input.agent_id.has_value() && !input.agent_id.value().empty()) {
    auto agent = AgentRegistry::instance().getAgent(input.agent_id.value());
    if (!agent) {
      return shared::ToolResult::fail("Agent not found: " +
                                      input.agent_id.value());
    }

    // Check if agent is busy
    if (agent->getContext().state.currentStatus == AgentStatus::Streaming) {
      return shared::ToolResult::fail("Agent is busy");
    }

    // Re-task existing agent
    Engine::instance().executeTask(input.agent_id.value(), input.task);

    if (input.async) {
      rapidjson::Document d;
      d.SetObject();
      auto &a = d.GetAllocator();
      d.AddMember("agentId",
                  rapidjson::Value(input.agent_id.value().c_str(), a).Move(),
                  a);
      d.AddMember("status", "re-tasked", a);
      return shared::ToolResult::ok(d);
    } else {
      std::string resultSummary;
      while (true) {
        auto res = Engine::instance().waitForAgent(
            input.agent_id.value(), std::chrono::milliseconds(100));
        if (res.has_value()) {
          resultSummary = *res;
          break;
        }
        if (ctx.agent.isInterrupted()) {
          Engine::instance().cancelAgent(input.agent_id.value());
          return shared::ToolResult::fail(
              "Parent agent interrupted while waiting for subagent.");
        }
      }

      rapidjson::Document d;
      d.SetObject();
      auto &a = d.GetAllocator();
      d.AddMember("agentId",
                  rapidjson::Value(input.agent_id.value().c_str(), a).Move(),
                  a);
      if (resultSummary.find("Error:") == 0) {
        return shared::ToolResult::fail(resultSummary);
      }
      d.AddMember("status", "completed", a);
      d.AddMember("result", rapidjson::Value(resultSummary.c_str(), a).Move(),
                  a);
      return shared::ToolResult::ok(d);
    }
  }

  // Original behavior: summon new agent
  if (input.async) {
    std::string subagentId = Engine::instance().summonAgent(
        threadId, input.persona, input.task, true,
        ctx.agent.getContext().identity.id, input.name, input.title);
    rapidjson::Document d;
    d.SetObject();
    auto &a = d.GetAllocator();
    d.AddMember("agentId", rapidjson::Value(subagentId.c_str(), a).Move(), a);
    d.AddMember("status", "spawned", a);
    return shared::ToolResult::ok(d);
  } else {
    std::string subagentId = Engine::instance().summonAgent(
        threadId, input.persona, input.task, true,
        ctx.agent.getContext().identity.id, input.name, input.title);

    // Polling wait to support heartbeats and interrupts
    std::string resultSummary;
    while (true) {
      auto res = Engine::instance().waitForAgent(
          subagentId, std::chrono::milliseconds(100));
      if (res.has_value()) {
        resultSummary = *res;
        break;
      }
      if (ctx.agent.isInterrupted()) {
        Engine::instance().cancelAgent(subagentId);
        return shared::ToolResult::fail(
            "Parent agent interrupted while waiting for subagent.");
      }
    }

    rapidjson::Document d;
    d.SetObject();
    auto &a = d.GetAllocator();
    d.AddMember("agentId", rapidjson::Value(subagentId.c_str(), a).Move(), a);
    if (resultSummary.find("Error:") == 0) {
      return shared::ToolResult::fail(resultSummary);
    }
    d.AddMember("status", "completed", a);
    d.AddMember("result", rapidjson::Value(resultSummary.c_str(), a).Move(), a);
    return shared::ToolResult::ok(d);
  }
}

} // namespace firmius::core

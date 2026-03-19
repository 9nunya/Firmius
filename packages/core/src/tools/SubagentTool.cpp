#include "tools/SubagentTool.hpp"
#include "agents/PurposeLoader.hpp"
#include "AgentRegistry.hpp"
#include "Engine.hpp"
#include "Events.hpp"
#include "persistence/ThreadManager.hpp"
#include "tools/WorkToolCommon.hpp"
#include "utils/StringUtil.hpp"
#include <sstream>

namespace firmius::core {

namespace {

bool isExecutorPersona(const std::string &persona) { return persona == "executor"; }

bool isWorkerPersona(const std::string &persona) {
  return persona == "worker" || persona == "scout";
}

const shared::WorkChunk &
findChunk(const shared::Plan &plan, const std::string &chunkId) {
  return worktools::requireChunk(plan, chunkId);
}

shared::Plan loadPlan(const std::string &threadId, const std::string &planId) {
  ThreadManager tm(std::string(getenv("HOME") ? getenv("HOME") : "/tmp") +
                   "/.firmius/threads");
  return tm.getPlan(threadId, planId);
}

std::string buildExecutorTask(const shared::Plan &plan,
                              const shared::WorkChunk &chunk,
                              const std::string &task) {
  std::ostringstream prompt;
  prompt << "You are the executor responsible for exactly one assigned work "
            "chunk.\n\n";
  prompt << "Chunk Ownership Contract\n";
  prompt << "- You own exactly this chunk: " << chunk.id << "\n";
  prompt << "- You do not own the whole plan.\n";
  prompt << "- Do not take ownership of any other chunk.\n";
  prompt << "- Stay inside this chunk's scope. Do not silently repair sibling "
            "chunks or broader architecture.\n";
  prompt << "- If you discover an upstream, downstream, or cross-cutting "
            "problem outside this chunk, report it explicitly instead of "
            "claiming you fixed the plan.\n";
  prompt << "- You may delegate only bounded subtasks to worker/scout helpers "
            "one level deep.\n\n";
  prompt << "Plan Context\n";
  prompt << "Plan Title: " << plan.title << "\n";
  prompt << "Plan Objective: " << plan.objective << "\n";
  prompt << "Plan Strategy Summary: " << plan.strategy << "\n\n";
  prompt << "Assigned Chunk\n";
  prompt << "Chunk ID: " << chunk.id << "\n";
  prompt << "Chunk Title: " << chunk.title << "\n";
  prompt << "Chunk Goal: " << chunk.goal << "\n";
  prompt << "Chunk Context: " << chunk.context << "\n";
  prompt << "Chunk Constraints: " << chunk.constraints << "\n";
  prompt << "Chunk Completion: " << chunk.completion << "\n";
  prompt << "\nExecution Discipline\n";
  prompt << "- Reread the exact files and anchors you touch before editing.\n";
  prompt << "- If an anchor or local context is stale, reread and repair it "
            "before editing; do not guess.\n";
  prompt << "- Do not broaden the task because a nearby cleanup looks tempting.\n";
  prompt << "- Do not claim completion, verification, or review without "
            "evidence.\n";
  prompt << "- Only the lead accepts work and marks a chunk Done after review; "
            "your terminal success state is normally Implemented.\n";
  prompt << "\nVerification Expectations\n";
  prompt << "- Run the concrete verification needed for this chunk. Prefer the "
            "narrowest checks that still produce real evidence.\n";
  prompt << "- Your report must name the verification commands or tests you ran "
            "and the outcome.\n";
  prompt << "- If verification is blocked or incomplete, say exactly why. "
            "Do not write 'looks correct' or equivalent guesswork.\n";
  prompt << "\nExecution State Reporting\n";
  prompt << "- If you report chunk progress with chunk_update, use plan_id=\""
         << plan.id << "\" and chunk_id=\"" << chunk.id << "\".\n";
  prompt << "- The only chunk fields you may write are: status, attempt_count, result_summary.\n";
  prompt << "- Valid chunk_update payload pattern: {\"plan_id\":\"" << plan.id
         << "\",\"chunk_id\":\"" << chunk.id
         << "\",\"status\":\"Implemented\",\"attempt_count\":1,\"result_summary\":\"implemented scoped changes; verified with focused evidence\"}.\n";
  prompt << "- Do not send title, goal, context, constraints, completion, depends_on, assigned_agent_id, or review_summary through chunk_update.\n";
  prompt << "- Any design, review, dependency, or assignment fields in chunk_update will be rejected by runtime authority checks.\n";
  prompt << "- Treat execution dispatch as already started for this chunk; update status/result only when you have real progress to report.\n";
  prompt << "- Use Implemented when the chunk changes are complete as far as you can take them with evidence.\n";
  prompt << "- Use Blocked or Failed when you hit a real blocker; say what blocked you.\n";
  prompt << "- Do not mark the chunk Done yourself.\n";
  prompt << "- Report back in a compact structure the lead can review quickly:\n";
  prompt << "  Changed: <files/behavior>\n";
  prompt << "  Verified: <command/test and result>\n";
  prompt << "  Blockers/Risks: <none or concrete issue>\n";
  if (!task.empty()) {
    prompt << "\nLead Notes\n" << task << "\n";
  }
  return prompt.str();
}

std::string buildWorkerTask(const std::string &task) {
  std::ostringstream prompt;
  prompt << "You are a worker helper supporting your parent executor on a "
            "bounded subtask.\n\n";
  prompt << "Boundaries\n";
  prompt << "- You do not own a plan chunk.\n";
  prompt << "- You are not responsible for the whole plan.\n";
  prompt << "- Complete only the bounded subtask below and return useful "
            "results to the executor.\n\n";
  prompt << "Subtask\n" << task << "\n";
  return prompt.str();
}

void ensureExecutorAssignmentAvailable(const std::string &threadId,
                                       const std::string &planId,
                                       const std::string &chunkId,
                                       const std::optional<std::string> &agentId) {
  ThreadManager tm(std::string(getenv("HOME") ? getenv("HOME") : "/tmp") +
                   "/.firmius/threads");
  const shared::Plan plan = tm.getPlan(threadId, planId);
  const shared::WorkChunk &chunk = findChunk(plan, chunkId);

  if (!chunk.assignedAgentId.empty() &&
      (!agentId.has_value() || chunk.assignedAgentId != *agentId)) {
    throw std::runtime_error("Chunk '" + chunkId +
                             "' is already owned by executor agent '" +
                             chunk.assignedAgentId + "'");
  }

  if (!agentId.has_value() || agentId->empty()) {
    return;
  }

  for (const auto &candidatePlan : tm.listPlans(threadId)) {
    for (const auto &candidateChunk : candidatePlan.chunks) {
      if (candidateChunk.assignedAgentId != *agentId) {
        continue;
      }
      if (candidatePlan.id == planId && candidateChunk.id == chunkId) {
        continue;
      }
      throw std::runtime_error("Executor agent '" + *agentId +
                               "' already owns chunk '" + candidateChunk.id +
                               "'");
    }
  }
}

void ensureExecutorChunkReadyForDispatch(const std::string &threadId,
                                         const std::string &planId,
                                         const std::string &chunkId) {
  ThreadManager tm(std::string(getenv("HOME") ? getenv("HOME") : "/tmp") +
                   "/.firmius/threads");
  shared::Plan plan = tm.getPlan(threadId, planId);
  auto &chunk = worktools::requireChunk(plan, chunkId);
  const shared::WorkChunk originalChunk = chunk;
  if (worktools::blockChunkIfDependenciesIncomplete(plan, chunk)) {
    chunk.updatedAt = worktools::nowEpochMs();
    tm.updatePlan(threadId, plan);
    worktools::emitWorkEvent(shared::ChunkUpdated{threadId, plan.id, chunk});
    worktools::emitWorkEvent(shared::ChunkStatusChanged{
        threadId, plan.id, chunk.id, originalChunk.status, chunk.status, chunk});
  }
  worktools::requireChunkReadyForExecution(plan, chunk, "dispatch");
}

void persistExecutorDispatch(const std::string &threadId,
                             const std::string &planId,
                             const std::string &chunkId,
                             const std::string &agentId) {
  ThreadManager tm(std::string(getenv("HOME") ? getenv("HOME") : "/tmp") +
                   "/.firmius/threads");
  shared::Plan plan = tm.getPlan(threadId, planId);
  auto &chunk = worktools::requireChunk(plan, chunkId);
  const shared::WorkChunk originalChunk = chunk;
  worktools::validateExecutorAssignmentInvariant(tm, threadId, plan.id, chunk.id,
                                                 agentId);
  chunk.assignedAgentId = agentId;
  if (chunk.status == shared::WorkChunkStatus::Ready) {
    chunk.status = shared::WorkChunkStatus::InProgress;
  }
  chunk.updatedAt = worktools::nowEpochMs();
  tm.updatePlan(threadId, plan);
  worktools::emitWorkEvent(shared::ChunkUpdated{threadId, plan.id, chunk});
  if (originalChunk.assignedAgentId != chunk.assignedAgentId) {
    worktools::emitWorkEvent(shared::ChunkAssigned{
        threadId, plan.id, chunk.id, chunk.assignedAgentId, chunk});
  }
  if (originalChunk.status != chunk.status) {
    worktools::emitWorkEvent(shared::ChunkStatusChanged{
        threadId, plan.id, chunk.id, originalChunk.status, chunk.status,
        chunk});
  }
}

std::string buildDelegationTask(const SubagentInput &input,
                                const std::string &threadId) {
  if (isExecutorPersona(input.persona) && input.plan_id.has_value() &&
      input.chunk_id.has_value()) {
    const shared::Plan plan = loadPlan(threadId, *input.plan_id);
    const shared::WorkChunk &chunk = findChunk(plan, *input.chunk_id);
    return buildExecutorTask(plan, chunk, input.task);
  }

  if (isWorkerPersona(input.persona)) {
    return buildWorkerTask(input.task);
  }

  return input.task;
}

}

shared::ToolMetadata SubagentTool::getMetadata() const {
  return {"summon_subagent", "Summon a child agent to perform a sub-task.",
          shared::ToolScope::Delegation};
}

std::shared_ptr<shared::JSONSchema> SubagentTool::getSchema() const {
  return shared::zObject(
             {{"persona", shared::zString()->describe(
                              "Persona name (e.g., 'executor', 'worker', 'scout')")},
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
              {"plan_id",
               shared::zString()
                   ->describe("Optional plan to bind delegation context to")
                   ->setOptional()},
              {"chunk_id",
               shared::zString()
                   ->describe("Optional assigned chunk for executor delegation")
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
  std::string persona;
  try {
    const std::string loweredPersona =
        shared::StringUtil::toLower(shared::StringUtil::trim(input.persona));
    if (loweredPersona == "implementer" || loweredPersona == "researcher") {
      persona = worktools::normalizePersonaRole(input.persona, "persona");
    } else if (input.plan_id.has_value() || input.chunk_id.has_value()) {
      persona = worktools::normalizePersonaRole(input.persona, "persona");
    } else {
      persona = loweredPersona;
    }
  } catch (const std::exception &e) {
    return shared::ToolResult::fail(e.what());
  }

  if (!PurposeLoader::isValid(persona)) {
    return shared::ToolResult::fail("Invalid persona: '" + input.persona +
                                    "'. Check available personas in base.md or prompts/ directory.");
  }

  std::string threadId = ctx.agent.getContext().history->threadId;
  if (input.plan_id.has_value() != input.chunk_id.has_value()) {
    return shared::ToolResult::fail(
        "plan_id and chunk_id must either both be provided or both be omitted");
  }

  if (isExecutorPersona(persona) && input.plan_id.has_value() &&
      input.chunk_id.has_value()) {
    try {
      ensureExecutorChunkReadyForDispatch(threadId, *input.plan_id,
                                          *input.chunk_id);
      ensureExecutorAssignmentAvailable(threadId, *input.plan_id, *input.chunk_id,
                                        input.agent_id);
    } catch (const std::exception &e) {
      return shared::ToolResult::fail(e.what());
    }
  }

  const std::string delegatedTask = [&]() -> std::string {
    try {
      SubagentInput normalizedInput = input;
      normalizedInput.persona = persona;
      return buildDelegationTask(normalizedInput, threadId);
    } catch (const std::exception &e) {
      throw;
    }
  }();

  auto existingAgents = AgentRegistry::instance().listAll();
  for (const auto &agentId : existingAgents) {
    auto agent = AgentRegistry::instance().getAgent(agentId);
    if (agent && agent->getContext().identity.friendlyName == input.name &&
        (!input.agent_id.has_value() || agentId != *input.agent_id)) {
      return shared::ToolResult::fail("Agent name '" + input.name +
                                      "' already exists in this thread");
    }
  }

  // Inherit model from parent agent
  std::string providerId = ctx.agent.getContext().config.providerId;
  std::string modelId = ctx.agent.getContext().config.modelId;
  std::string variantName = ctx.agent.getContext().config.modelVariant;

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
    if (isExecutorPersona(persona) && input.plan_id.has_value() &&
        input.chunk_id.has_value()) {
      persistExecutorDispatch(threadId, *input.plan_id, *input.chunk_id,
                              input.agent_id.value());
    }
    Engine::instance().executeTask(input.agent_id.value(), delegatedTask);

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
            input.agent_id.value(), std::chrono::milliseconds(20));
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
        threadId, persona, delegatedTask, true,
        ctx.agent.getContext().identity.id, input.name, input.title, "",
        providerId, modelId, variantName);
    if (isExecutorPersona(persona) && input.plan_id.has_value() &&
        input.chunk_id.has_value()) {
      persistExecutorDispatch(threadId, *input.plan_id, *input.chunk_id,
                              subagentId);
    }
    rapidjson::Document d;
    d.SetObject();
    auto &a = d.GetAllocator();
    d.AddMember("agentId", rapidjson::Value(subagentId.c_str(), a).Move(), a);
    d.AddMember("status", "spawned", a);
    return shared::ToolResult::ok(d);
  } else {
    std::string subagentId = Engine::instance().summonAgent(
        threadId, persona, delegatedTask, true,
        ctx.agent.getContext().identity.id, input.name, input.title, "",
        providerId, modelId, variantName);
    if (isExecutorPersona(persona) && input.plan_id.has_value() &&
        input.chunk_id.has_value()) {
      persistExecutorDispatch(threadId, *input.plan_id, *input.chunk_id,
                              subagentId);
    }

    // Polling wait to support heartbeats and interrupts
    std::string resultSummary;
    while (true) {
      auto res = Engine::instance().waitForAgent(
          subagentId, std::chrono::milliseconds(20));
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

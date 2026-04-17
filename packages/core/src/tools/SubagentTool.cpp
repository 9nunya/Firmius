#include "tools/SubagentTool.hpp"
#include "agents/PurposeLoader.hpp"
#include "AgentRegistry.hpp"
#include "ConfigLoader.hpp"
#include "Engine.hpp"
#include "Events.hpp"
#include "Serialization.hpp"
#include "artifacts/ReferenceExpansion.hpp"
#include "persistence/ThreadManager.hpp"
#include "tools/WorkToolCommon.hpp"
#include "utils/StringUtil.hpp"
#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>
#include <chrono>
#include <filesystem>
#include <sstream>
#include <unordered_set>

namespace firmius::core {

namespace {

PurposeWorkRole purposeRoleForPersona(const std::string &persona) {
  return PurposeLoader::resolveWorkRole(persona);
}

std::optional<std::string> legacyPersonaSuggestion(const std::string &persona) {
  const std::string lowered = shared::StringUtil::toLower(
      shared::StringUtil::trim(persona));
  if (lowered == "implementer") {
    return "legacy role 'implementer'; use 'executor'";
  }
  if (lowered == "researcher") {
    return "legacy role 'researcher'; use 'scout'";
  }
  return std::nullopt;
}

bool isExecutorRole(PurposeWorkRole role) {
  return role == PurposeWorkRole::Executor;
}

bool isAuditorRole(PurposeWorkRole role) {
  return role == PurposeWorkRole::Auditor;
}

bool isWorkerLikeRole(PurposeWorkRole role) {
  return role == PurposeWorkRole::Worker || role == PurposeWorkRole::Scout;
}

std::optional<std::string> normalizeOptionalString(
    const std::optional<std::string> &value) {
  if (!value.has_value()) {
    return std::nullopt;
  }
  const std::string trimmed = shared::StringUtil::trim(*value);
  if (trimmed.empty()) {
    return std::nullopt;
  }
  return trimmed;
}

bool callerMayUseDreamMode(const shared::ToolContext &ctx) {
  const std::string lowered = shared::StringUtil::toLower(
      shared::StringUtil::trim(ctx.agent.getContext().config.personaName));
  return lowered == "lead" || lowered == "fast" || lowered == "hotrun";
}

bool isRetryableWaitOutcome(const AgentOutcome &outcome) {
  return outcome.kind == AgentOutcome::Kind::NoSummary ||
         outcome.kind == AgentOutcome::Kind::Failed;
}

std::string normalizeRouteToken(const std::string& value) {
  return shared::StringUtil::toLower(shared::StringUtil::trim(value));
}

bool userExplicitlyRequestedCategory(const shared::ToolContext& ctx,
                                     const std::string& category) {
  const auto& agentContext = ctx.agent.getContext();
  if (!agentContext.history) {
    return false;
  }

  const std::string normalizedCategory = normalizeRouteToken(category);
  if (normalizedCategory.empty()) {
    return false;
  }

  for (auto turnIt = agentContext.history->turns.rbegin();
       turnIt != agentContext.history->turns.rend(); ++turnIt) {
    for (auto msgIt = turnIt->messages.rbegin();
         msgIt != turnIt->messages.rend(); ++msgIt) {
      if (msgIt->role != shared::Role::User) {
        continue;
      }
      for (const auto& part : msgIt->content) {
        if (const auto* txt = std::get_if<shared::TextContent>(&part)) {
          const std::string lowered =
              normalizeRouteToken(txt->text);
          if (lowered.find(normalizedCategory) != std::string::npos) {
            return true;
          }
        }
      }
    }
  }
  return false;
}

const shared::WorkChunk &
findChunk(const shared::Plan &plan, const std::string &chunkId) {
  return worktools::requireChunk(plan, chunkId);
}

shared::Plan loadPlan(const std::string &threadId, const std::string &planId) {
  ThreadManager tm(ThreadManager::defaultBasePath());
  return tm.getPlan(threadId, planId);
}

std::optional<shared::Plan> loadRequestedOrActivePlan(
    const std::string &threadId,
    const std::optional<std::string> &requestedPlanId) {
  ThreadManager tm(ThreadManager::defaultBasePath());
  if (requestedPlanId.has_value() && !requestedPlanId->empty()) {
    return tm.getPlan(threadId, *requestedPlanId);
  }
  const auto metadata = tm.getMetadata(threadId);
  if (!metadata.activePlanId.empty()) {
    return tm.getPlan(threadId, metadata.activePlanId);
  }
  return std::nullopt;
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
  
  // V2 rich chunk spec fields (only if present)
  if (!chunk.filesToRead.empty()) {
    prompt << "Files To Read: ";
    for (size_t i = 0; i < chunk.filesToRead.size(); ++i) {
      if (i > 0) prompt << ", ";
      prompt << chunk.filesToRead[i];
    }
    prompt << "\n";
  }
  if (!chunk.filesToTouch.empty()) {
    prompt << "Files To Touch: ";
    for (size_t i = 0; i < chunk.filesToTouch.size(); ++i) {
      if (i > 0) prompt << ", ";
      prompt << chunk.filesToTouch[i];
    }
    prompt << "\n";
  }
  if (!chunk.cwd.empty()) {
    prompt << "Working Directory: " << chunk.cwd << "\n";
  }
  if (!chunk.verificationCondition.empty()) {
    prompt << "Verification Condition: " << chunk.verificationCondition << "\n";
  }
  if (!chunk.handoffNotes.empty()) {
    prompt << "Handoff Notes: " << chunk.handoffNotes << "\n";
  }

  // V2 chunk-internal task structure (only if present)
  if (!chunk.tasks.empty()) {
    prompt << "\nChunk Tasks\n";
    prompt << "This chunk contains " << chunk.tasks.size() << " internal tasks. ";
    prompt << "Use these to structure your execution or delegate to workers:\n";
    for (const auto &t : chunk.tasks) {
      std::string statusLabel;
      switch (t.status) {
      case shared::WorkChunkStatus::Ready:
        statusLabel = "Ready";
        break;
      case shared::WorkChunkStatus::InProgress:
        statusLabel = "InProgress";
        break;
      case shared::WorkChunkStatus::Implemented:
        statusLabel = "Implemented";
        break;
      case shared::WorkChunkStatus::Verifying:
        statusLabel = "Verifying";
        break;
      case shared::WorkChunkStatus::Done:
        statusLabel = "Done";
        break;
      case shared::WorkChunkStatus::Blocked:
        statusLabel = "Blocked";
        break;
      case shared::WorkChunkStatus::Failed:
        statusLabel = "Failed";
        break;
      case shared::WorkChunkStatus::Cancelled:
        statusLabel = "Cancelled";
        break;
      }
      prompt << "- [" << statusLabel << "] " << t.title;
      if (!t.goal.empty()) {
        prompt << ": " << t.goal;
      }
      if (!t.notes.empty()) {
        prompt << " (Note: " << t.notes << ")";
      }
      if (!t.verificationCondition.empty()) {
        prompt << " (Verify: " << t.verificationCondition << ")";
      }
      prompt << "\n";
    }
  }

  prompt << "\nExecution Discipline\n";
  prompt << "- Reread the exact files and anchors you touch before editing.\n";
  prompt << "- If the target directory or files do not exist yet, that is not "
            << "a blocker for greenfield chunk work; create the first scoped "
            << "files directly with file_edit content.\n";
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
  
  prompt << "\n" << worktools::buildExecutorLockDoctrine() << "\n";
  
  if (!task.empty()) {
    prompt << "\nLead Notes\n" << task << "\n";
  }
  return prompt.str();
}

std::string buildWorkerTask(const std::string &task,
                            const std::optional<shared::WorkTask> &workTask) {
  std::ostringstream prompt;
  prompt << "You are a worker helper supporting your parent executor on a "
            "bounded subtask.\n\n";
  prompt << "Boundaries\n";
  prompt << "- You do not own a plan chunk.\n";
  prompt << "- You are not responsible for the whole plan.\n";
  prompt << "- Complete only the bounded subtask below and return useful "
            "results to the executor.\n\n";
  
  if (workTask.has_value()) {
    prompt << "Assigned Task\n";
    prompt << "Task ID: " << workTask->id << "\n";
    prompt << "Task Title: " << workTask->title << "\n";
    prompt << "Task Goal: " << workTask->goal << "\n";
    if (!workTask->notes.empty()) {
      prompt << "Task Notes: " << workTask->notes << "\n";
    }
    if (!workTask->verificationCondition.empty()) {
      prompt << "Verification: " << workTask->verificationCondition << "\n";
    }
    prompt << "\n";
  }
  
  prompt << "Subtask\n" << task << "\n";
  
  prompt << "\n" << worktools::buildWorkerLockDoctrine() << "\n";
  
  return prompt.str();
}

std::string buildAuditorTask(const shared::Plan &plan,
                             const shared::WorkChunk &chunk,
                             const std::string &task) {
  std::ostringstream prompt;
  prompt << "You are the auditor responsible for evidence-backed review of a "
            "single work chunk.\n\n";
  prompt << "Auditor Contract\n";
  prompt << "- You do NOT own execution of this chunk.\n";
  prompt << "- Verify implementation and review evidence; do not implement.\n";
  prompt << "- Report a clear verdict with concrete evidence or identified gaps.\n";
  prompt << "- If evidence is missing or incomplete, say exactly what is missing.\n\n";

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
  prompt << "Chunk Status: " << worktools::chunkStatusToString(chunk.status)
         << "\n";
  if (!chunk.assignedAgentId.empty()) {
    prompt << "Assigned Executor: " << chunk.assignedAgentId << "\n";
  }
  if (!chunk.resultSummary.empty()) {
    prompt << "Result Summary: " << chunk.resultSummary << "\n";
  }
  if (!chunk.reviewSummary.empty()) {
    prompt << "Review Summary: " << chunk.reviewSummary << "\n";
  }

  // V2 rich chunk spec fields (only if present)
  if (!chunk.filesToRead.empty()) {
    prompt << "Files To Read: ";
    for (size_t i = 0; i < chunk.filesToRead.size(); ++i) {
      if (i > 0) prompt << ", ";
      prompt << chunk.filesToRead[i];
    }
    prompt << "\n";
  }
  if (!chunk.filesToTouch.empty()) {
    prompt << "Files To Touch: ";
    for (size_t i = 0; i < chunk.filesToTouch.size(); ++i) {
      if (i > 0) prompt << ", ";
      prompt << chunk.filesToTouch[i];
    }
    prompt << "\n";
  }
  if (!chunk.cwd.empty()) {
    prompt << "Working Directory: " << chunk.cwd << "\n";
  }
  if (!chunk.verificationCondition.empty()) {
    prompt << "Verification Condition: " << chunk.verificationCondition << "\n";
  }
  if (!chunk.handoffNotes.empty()) {
    prompt << "Handoff Notes: " << chunk.handoffNotes << "\n";
  }

  if (!chunk.tasks.empty()) {
    prompt << "\nChunk Tasks\n";
    prompt << "This chunk contains " << chunk.tasks.size()
           << " internal tasks. Use these to anchor review evidence:\n";
    for (const auto &t : chunk.tasks) {
      std::string statusLabel;
      switch (t.status) {
      case shared::WorkChunkStatus::Ready:
        statusLabel = "Ready";
        break;
      case shared::WorkChunkStatus::InProgress:
        statusLabel = "InProgress";
        break;
      case shared::WorkChunkStatus::Implemented:
        statusLabel = "Implemented";
        break;
      case shared::WorkChunkStatus::Verifying:
        statusLabel = "Verifying";
        break;
      case shared::WorkChunkStatus::Done:
        statusLabel = "Done";
        break;
      case shared::WorkChunkStatus::Blocked:
        statusLabel = "Blocked";
        break;
      case shared::WorkChunkStatus::Failed:
        statusLabel = "Failed";
        break;
      case shared::WorkChunkStatus::Cancelled:
        statusLabel = "Cancelled";
        break;
      }
      prompt << "- [" << statusLabel << "] " << t.title;
      if (!t.goal.empty()) {
        prompt << ": " << t.goal;
      }
      if (!t.notes.empty()) {
        prompt << " (Note: " << t.notes << ")";
      }
      if (!t.verificationCondition.empty()) {
        prompt << " (Verify: " << t.verificationCondition << ")";
      }
      prompt << "\n";
    }
  }

  prompt << "\nReview Discipline\n";
  prompt << "- Prefer direct evidence: tests, logs, file diffs, or runtime checks.\n";
  prompt << "- Do not accept claims without evidence.\n";
  prompt << "- Provide an explicit verdict: accept, reject, or needs more evidence.\n";
  prompt << "- Report in a compact structure the lead can act on:\n";
  prompt << "  Verdict: <accept/reject/needs-evidence>\n";
  prompt << "  Evidence: <commands/tests/files>\n";
  prompt << "  Gaps/Risks: <none or concrete issue>\n";
  if (!task.empty()) {
    prompt << "\nLead Notes\n" << task << "\n";
  }
  return prompt.str();
}

void ensureExecutorAssignmentAvailable(const std::string &threadId,
                                       const std::string &planId,
                                       const std::string &chunkId,
                                       const std::optional<std::string> &agentId) {
  ThreadManager tm(ThreadManager::defaultBasePath());
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
  ThreadManager tm(ThreadManager::defaultBasePath());
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
  ThreadManager tm(ThreadManager::defaultBasePath());
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
                                const std::string &threadId,
                                PurposeWorkRole role) {
  if (isExecutorRole(role) && input.plan_id.has_value() &&
      input.chunk_id.has_value()) {
    const shared::Plan plan = loadPlan(threadId, *input.plan_id);
    const shared::WorkChunk &chunk = findChunk(plan, *input.chunk_id);
    return buildExecutorTask(plan, chunk, input.task);
  }

  if (isAuditorRole(role) && input.plan_id.has_value() &&
      input.chunk_id.has_value()) {
    const shared::Plan plan = loadPlan(threadId, *input.plan_id);
    const shared::WorkChunk &chunk = findChunk(plan, *input.chunk_id);
    return buildAuditorTask(plan, chunk, input.task);
  }

  if (isWorkerLikeRole(role) && input.task_id.has_value() &&
      !input.task_id->empty() && input.plan_id.has_value() &&
      input.chunk_id.has_value()) {
    const shared::Plan plan = loadPlan(threadId, *input.plan_id);
    const shared::WorkChunk &chunk = findChunk(plan, *input.chunk_id);
    
    // Find the task
    auto taskIt = std::find_if(chunk.tasks.begin(), chunk.tasks.end(),
                                [&](const shared::WorkTask &t) {
                                  return t.id == *input.task_id;
                                });
    if (taskIt != chunk.tasks.end()) {
      return buildWorkerTask(input.task, *taskIt);
    }
  }

  if (isWorkerLikeRole(role)) {
    return buildWorkerTask(input.task, std::nullopt);
  }

  return input.task;
}

std::string buildDreamerTask(const SubagentInput &input,
                             const std::string &threadId,
                             const shared::ToolContext &ctx,
                             const std::string &memoryRoot) {
  std::ostringstream prompt;
  prompt << "You are being summoned in restricted dream mode by a lead agent.\n\n";
  prompt << "Dream Sandbox\n";
  prompt << "- Working directory: " << memoryRoot << "\n";
  prompt << "- Read/write only under that directory.\n";
  prompt << "- Do not modify the project repository itself.\n";
  prompt << "- Prefer USER.md, BEHAVIOR.md, and project-specific notes under projects/.\n\n";

  const auto &parentCtx = ctx.agent.getContext();
  prompt << "Lead Context\n";
  prompt << "- Parent persona: " << parentCtx.config.personaName << "\n";
  prompt << "- Source workspace: " << parentCtx.environment.cwd << "\n";
  prompt << "- Thread ID: " << threadId << "\n\n";

  if (auto plan = loadRequestedOrActivePlan(threadId, input.plan_id);
      plan.has_value()) {
    prompt << "Plan Context\n";
    prompt << "- Title: " << plan->title << "\n";
    prompt << "- Objective: " << plan->objective << "\n";
    prompt << "- Strategy: " << plan->strategy << "\n";
    prompt << "- Chunks:\n";
    for (const auto &chunk : plan->chunks) {
      prompt << "  - " << chunk.title << " (" << chunk.id << ")\n";
    }
    prompt << "\n";
  }

  prompt << "Lead Dream Request\n" << input.task << "\n";
  return prompt.str();
}

struct ResolvedRoute {
  std::string providerId;
  std::string modelId;
  std::string variantName;
  std::string categoryName;
  std::string warning;
};

ResolvedRoute resolveModelRoute(const std::string &persona,
                                const std::optional<std::string>& explicitCategoryOverride = std::nullopt,
                                const std::string& explicitCategoryWarning = "") {
  const auto &config = shared::ConfigLoader::instance().getConfig();

  auto useDefaultRoute = [&config]() {
    ResolvedRoute route;
    route.providerId = config.defaultProviderId;
    route.modelId = config.defaultModelId;
    route.variantName = config.defaultModelVariant;
    return route;
  };

  auto findCategory = [&config](const std::string &name)
      -> const shared::ModelRouteCategory * {
    auto it = config.modelRouterCategories.find(name);
    if (it == config.modelRouterCategories.end()) {
      return nullptr;
    }
    return &it->second;
  };

  if (explicitCategoryOverride.has_value() && !explicitCategoryOverride->empty()) {
    if (const auto *category = findCategory(*explicitCategoryOverride)) {
      if (!category->models.empty()) {
        const auto &opt = category->models.front();
        return {opt.providerId, opt.modelId, opt.variantName,
                *explicitCategoryOverride, explicitCategoryWarning};
      }
    }
    auto route = useDefaultRoute();
    route.warning = "Category '" + *explicitCategoryOverride +
                    "' not found; using default model route.";
    return route;
  }

  auto it_purpose = config.purposeRoutes.find(persona);
  if (it_purpose != config.purposeRoutes.end() && !it_purpose->second.empty()) {
    const std::string mapped_category = it_purpose->second;
    if (const auto *category = findCategory(mapped_category)) {
      if (!category->models.empty()) {
        const auto &opt = category->models.front();
        return {opt.providerId, opt.modelId, opt.variantName,
                mapped_category, ""};
      }
    }
    auto route = useDefaultRoute();
    route.warning = "Purpose route for '" + persona + "' points to missing category '" +
                    mapped_category + "'; using default model route.";
    return route;
  }

  if (!config.defaultRouteCategory.empty()) {
    if (const auto *category = findCategory(config.defaultRouteCategory)) {
      if (!category->models.empty()) {
        const auto &opt = category->models.front();
        return {opt.providerId, opt.modelId, opt.variantName,
                config.defaultRouteCategory, ""};
      }
    }
  }

  return useDefaultRoute();
}

std::string routeLabel(const ResolvedRoute &route) {
  return route.categoryName.empty() ? "default" : route.categoryName;
}

std::vector<ResolvedRoute> buildRouteCandidates(const std::string &persona,
                                                const std::optional<std::string>& explicitCategoryOverride = std::nullopt,
                                                const std::string& explicitCategoryWarning = "") {
  const auto &config = shared::ConfigLoader::instance().getConfig();
  std::vector<ResolvedRoute> routes;
  std::unordered_set<std::string> seen;
  auto pushUnique = [&](const ResolvedRoute &route) {
    std::string key = route.providerId + "|" + route.modelId + "|" +
                      route.variantName + "|" + route.categoryName;
    if (seen.insert(key).second) {
      routes.push_back(route);
    }
  };

  ResolvedRoute primary =
      resolveModelRoute(persona, explicitCategoryOverride, "");
  if (!explicitCategoryWarning.empty()) {
    if (primary.warning.empty()) {
      primary.warning = explicitCategoryWarning;
    } else {
      primary.warning = explicitCategoryWarning + " " + primary.warning;
    }
  }
  pushUnique(primary);

  if (!config.enableSubagentRouteFallback) {
    return routes;
  }

  std::vector<std::string> fallbackCategories = config.subagentRouteFallbackOrder;
  if (fallbackCategories.empty()) {
    for (const auto &[name, _] : config.modelRouterCategories) {
      fallbackCategories.push_back(name);
    }
  }

  for (const auto &name : fallbackCategories) {
    auto it = config.modelRouterCategories.find(name);
    if (it == config.modelRouterCategories.end() || it->second.models.empty()) {
      continue;
    }
    const auto &opt = it->second.models.front();
    pushUnique(ResolvedRoute{opt.providerId, opt.modelId,
                             opt.variantName, name, ""});
  }
  return routes;
}

void appendRoutingMetadata(rapidjson::Document &d, const ResolvedRoute &route,
                           const std::vector<std::string> &attemptedCategories,
                           bool fallbackUsed) {
  auto &a = d.GetAllocator();
  if (!route.categoryName.empty()) {
    d.AddMember("category", rapidjson::Value(route.categoryName.c_str(), a).Move(),
                a);
  }
  rapidjson::Value attempted(rapidjson::kArrayType);
  for (const auto &category : attemptedCategories) {
    attempted.PushBack(rapidjson::Value(category.c_str(), a).Move(), a);
  }
  d.AddMember("attempted_categories", attempted, a);
  d.AddMember("fallback_used", fallbackUsed, a);
  if (!route.warning.empty()) {
    d.AddMember("routing_warning", rapidjson::Value(route.warning.c_str(), a).Move(),
                a);
  }
}

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
      const std::string owner = artifact.ownerFriendlyName.empty()
                                    ? artifact.ownerAgentId
                                    : artifact.ownerFriendlyName;
      const std::string reference = "@artifact:" + owner + "/" + artifact.filename;
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
                   ->describe("Optional chunk to bind for executor/auditor delegation")
                   ->setOptional()},
              {"task_id",
               shared::zString()
                   ->describe("Optional task ID within chunk to attach worker. Use when delegating to a worker for a specific subtask.")
                   ->setOptional()},
              {"category",
               shared::zString()
                   ->describe("Optional model routing category override. Use only "
                              "when the user explicitly requested a specific "
                              "route category; otherwise omit it so "
                              "purpose/default routing applies.")
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
  SubagentInput normalizedInput = input;
  normalizedInput.agent_id = normalizeOptionalString(input.agent_id);
  normalizedInput.plan_id = normalizeOptionalString(input.plan_id);
  normalizedInput.chunk_id = normalizeOptionalString(input.chunk_id);
  normalizedInput.task_id = normalizeOptionalString(input.task_id);
  normalizedInput.category = normalizeOptionalString(input.category);

  std::string persona;
  PurposeWorkRole workRole = PurposeWorkRole::Unknown;
  try {
    if (normalizedInput.dream) {
      if (!callerMayUseDreamMode(ctx)) {
        return shared::ToolResult::fail(
            "dream summon mode is restricted to lead, fast, or hotrun agents");
      }
      persona = "dreamer";
    } else {
      persona = shared::StringUtil::trim(input.persona);
    }
    workRole = purposeRoleForPersona(persona);
  } catch (const std::exception &e) {
    return shared::ToolResult::fail(e.what());
  }

  if (!PurposeLoader::isValid(persona)) {
    if (auto suggestion = legacyPersonaSuggestion(persona)) {
      return shared::ToolResult::fail("Invalid persona: '" + persona +
                                      "'. " + *suggestion + ".");
    }
    return shared::ToolResult::fail("Invalid persona: '" + persona +
                                    "'. Check available personas in base.md or prompts/ directory.");
  }

  std::string threadId = ctx.agent.getContext().history->threadId;
  if (normalizedInput.plan_id.has_value() != normalizedInput.chunk_id.has_value()) {
    return shared::ToolResult::fail(
        "plan_id and chunk_id must either both be provided or both be omitted");
  }

  // task_id requires both plan_id and chunk_id
  if (normalizedInput.task_id.has_value() && !normalizedInput.task_id->empty()) {
    if (!normalizedInput.plan_id.has_value() || !normalizedInput.chunk_id.has_value()) {
      return shared::ToolResult::fail(
          "task_id requires both plan_id and chunk_id to be provided");
    }
  }

  if (isExecutorRole(workRole) && normalizedInput.plan_id.has_value() &&
      normalizedInput.chunk_id.has_value()) {
    try {
      ensureExecutorChunkReadyForDispatch(threadId, *normalizedInput.plan_id,
                                          *normalizedInput.chunk_id);
      ensureExecutorAssignmentAvailable(threadId, *normalizedInput.plan_id,
                                        *normalizedInput.chunk_id,
                                        normalizedInput.agent_id);
    } catch (const std::exception &e) {
      return shared::ToolResult::fail(e.what());
    }
  }

  // Worker assignment to task: validate task exists and assign worker
  if (isWorkerLikeRole(workRole) && normalizedInput.task_id.has_value() &&
      !normalizedInput.task_id->empty() && normalizedInput.plan_id.has_value() &&
      normalizedInput.chunk_id.has_value()) {
    try {
      ThreadManager tm(ThreadManager::defaultBasePath());
      shared::Plan plan = tm.getPlan(threadId, *normalizedInput.plan_id);
      auto &chunk = worktools::requireChunk(plan, *normalizedInput.chunk_id);
      
      // Find the task
      auto taskIt = std::find_if(chunk.tasks.begin(), chunk.tasks.end(),
                                  [&](const shared::WorkTask &t) {
                                    return t.id == *normalizedInput.task_id;
                                  });
      if (taskIt == chunk.tasks.end()) {
        return shared::ToolResult::fail(
            "Task '" + *normalizedInput.task_id + "' not found in chunk '" +
            *normalizedInput.chunk_id + "'");
      }
      
      // Assign worker to task if not already assigned
      if (!taskIt->assignedWorkerId.empty() &&
          (!normalizedInput.agent_id.has_value() ||
           taskIt->assignedWorkerId != *normalizedInput.agent_id)) {
        return shared::ToolResult::fail(
            "Task '" + *normalizedInput.task_id +
            "' is already assigned to worker '" +
            taskIt->assignedWorkerId + "'");
      }
      
      // Persist worker assignment
      if (normalizedInput.agent_id.has_value() &&
          !normalizedInput.agent_id->empty()) {
        taskIt->assignedWorkerId = *normalizedInput.agent_id;
        taskIt->updatedAt = worktools::nowEpochMs();
        chunk.updatedAt = taskIt->updatedAt;
        tm.updatePlan(threadId, plan);
      }
    } catch (const std::exception &e) {
      return shared::ToolResult::fail(e.what());
    }
  }

  std::string delegatedTask;
  std::optional<SummonAgentOverrides> summonOverrides;
  try {
    normalizedInput.persona = persona;
    std::string task;
    if (normalizedInput.dream) {
      const std::string home = std::getenv("HOME") ? std::getenv("HOME") : "/root";
      const std::string memoryRoot = home + "/.firmius/user";
      std::filesystem::create_directories(memoryRoot);
      task = buildDreamerTask(normalizedInput, threadId, ctx, memoryRoot);
      summonOverrides = SummonAgentOverrides{
          .cwdOverride = memoryRoot,
          .allowedPathsOverride =
              std::vector<std::string>{memoryRoot, memoryRoot + "/**"}};
    } else {
      task = buildDelegationTask(normalizedInput, threadId, workRole);
    }
    const std::string cwd = ctx.agent.getContext().environment.cwd;
    delegatedTask =
        firmius::core::artifacts::expandInboundReferences(threadId, cwd, task);
  } catch (const std::exception &e) {
    return shared::ToolResult::fail("Reference expansion failed: " +
                                    std::string(e.what()));
  }

  auto existingAgents = AgentRegistry::instance().listAll();
  for (const auto &agentId : existingAgents) {
    auto agent = AgentRegistry::instance().getAgent(agentId);
    if (agent && agent->getContext().identity.friendlyName == normalizedInput.name &&
        (!normalizedInput.agent_id.has_value() || agentId != *normalizedInput.agent_id)) {
      return shared::ToolResult::fail("Agent name '" + normalizedInput.name +
                                      "' already exists in this thread");
    }
  }

  std::optional<std::string> explicitCategoryOverride;
  std::string explicitCategoryWarning;
  if (normalizedInput.category.has_value() && !normalizedInput.category->empty()) {
    if (userExplicitlyRequestedCategory(ctx, *normalizedInput.category)) {
      explicitCategoryOverride = normalizedInput.category;
    } else {
      explicitCategoryWarning =
          "Ignored explicit category '" + *normalizedInput.category +
          "' because only user-specified route-category overrides are honored; "
          "using configured purpose/default routing.";
    }
  }

  const std::vector<ResolvedRoute> routes = buildRouteCandidates(
      persona, explicitCategoryOverride, explicitCategoryWarning);
  std::vector<std::string> attemptedCategories;
  const bool isRetaskingExistingAgent =
      normalizedInput.agent_id.has_value() && !normalizedInput.agent_id->empty();
  std::string reusableAgentId = isRetaskingExistingAgent
                                   ? *normalizedInput.agent_id
                                   : shared::StringUtil::generateUuid();
  bool agentExists = isRetaskingExistingAgent;

  auto waitForOutcome = [&](const std::string &agentId)
      -> std::optional<AgentOutcome> {
    while (true) {
      auto outcome = Engine::instance().waitForAgentOutcome(
          agentId, std::chrono::milliseconds(20));
      if (outcome.has_value()) {
        return outcome;
      }
      if (ctx.cancelRequested()) {
        Engine::instance().cancelAgent(agentId);
        return AgentOutcome{AgentOutcome::Kind::Cancelled,
                            "Cancelled by parent."};
      }
    }
  };

  constexpr auto kAsyncOutcomeProbeWindow = std::chrono::milliseconds(250);
  auto waitForOutcomeWithTimeout = [&](const std::string &agentId,
                                       std::chrono::milliseconds timeout)
      -> std::optional<AgentOutcome> {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
      auto outcome = Engine::instance().peekAgentOutcome(
          agentId, std::chrono::milliseconds(20));
      if (outcome.has_value()) {
        return outcome;
      }
      if (ctx.cancelRequested()) {
        Engine::instance().cancelAgent(agentId);
        return AgentOutcome{AgentOutcome::Kind::Cancelled,
                            "Cancelled by parent."};
      }
    }
    return std::nullopt;
  };

  auto buildWaitResult = [&](const std::string &agentId, const ResolvedRoute &route,
                             const AgentOutcome &outcome, bool fallbackUsed)
      -> shared::ToolResult {
    rapidjson::Document d;
    d.SetObject();
    auto &a = d.GetAllocator();
    d.AddMember("agentId", rapidjson::Value(agentId.c_str(), a).Move(), a);
    appendRoutingMetadata(d, route, attemptedCategories, fallbackUsed);
    appendOutcomeArtifacts(d, outcome);

    if (outcome.kind == AgentOutcome::Kind::Cancelled) {
      d.AddMember("status", "cancelled", a);
      d.AddMember("result", rapidjson::Value(outcome.text.c_str(), a).Move(),
                  a);
      return shared::ToolResult::ok(d);
    }
    if (outcome.kind == AgentOutcome::Kind::Failed) {
      d.AddMember("status", "failed", a);
      d.AddMember("error", rapidjson::Value(outcome.text.c_str(), a).Move(),
                  a);
      return failWithStructuredData(d, outcome.text);
    }
    if (outcome.kind == AgentOutcome::Kind::NoSummary) {
      d.AddMember("status", "completed_no_summary", a);
      d.AddMember("result", rapidjson::Value(outcome.text.c_str(), a).Move(),
                  a);
      return shared::ToolResult::ok(d);
    }

    d.AddMember("status", "completed", a);
    d.AddMember("result", rapidjson::Value(outcome.text.c_str(), a).Move(), a);
    return shared::ToolResult::ok(d);
  };

  for (std::size_t i = 0; i < routes.size(); ++i) {
    const auto &route = routes[i];
    attemptedCategories.push_back(routeLabel(route));

    if (agentExists) {
      auto agent = AgentRegistry::instance().getAgent(reusableAgentId);
      if (!agent) {
        return shared::ToolResult::fail("Agent not found: " + reusableAgentId);
      }
      if (agent->getContext().state.currentStatus != AgentStatus::Idle) {
        // Wait briefly for agent to become Idle if it's transitioning (e.g. finishing up previous run)
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (agent->getContext().state.currentStatus != AgentStatus::Idle &&
               std::chrono::steady_clock::now() < deadline) {
          std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        if (agent->getContext().state.currentStatus != AgentStatus::Idle) {
          return shared::ToolResult::fail("Agent is busy (status: " + std::to_string(static_cast<int>(agent->getContext().state.currentStatus)) + ")");
        }
      }
      try {
        Engine::instance().switchAgentModel(reusableAgentId, route.providerId,
                                            route.modelId, route.variantName);
        Engine::instance().executeTask(reusableAgentId, delegatedTask);
      } catch (const std::exception &) {
        if (i + 1 < routes.size()) {
          continue;
        }
        return shared::ToolResult::fail(
            "Failed to launch subagent run on all configured routes.");
      }

      if (isExecutorRole(workRole) && normalizedInput.plan_id.has_value() &&
          normalizedInput.chunk_id.has_value()) {
        persistExecutorDispatch(threadId, *normalizedInput.plan_id,
                                *normalizedInput.chunk_id,
                                reusableAgentId);
      }

      if (input.async) {
        auto immediateOutcome =
            waitForOutcomeWithTimeout(reusableAgentId, kAsyncOutcomeProbeWindow);
        if (immediateOutcome.has_value()) {
          if (isRetryableWaitOutcome(*immediateOutcome) && i + 1 < routes.size()) {
            continue;
          }
          if (immediateOutcome->kind == AgentOutcome::Kind::Cancelled) {
            return buildWaitResult(reusableAgentId, route, *immediateOutcome,
                                   i > 0);
          }
        }

        rapidjson::Document d;
        d.SetObject();
        auto &a = d.GetAllocator();
        d.AddMember("agentId", rapidjson::Value(reusableAgentId.c_str(), a).Move(),
                    a);
        d.AddMember("status", "re-tasked", a);
        appendRoutingMetadata(d, route, attemptedCategories, i > 0);
        return shared::ToolResult::ok(d);
      }

      auto outcome = waitForOutcome(reusableAgentId);
      if (!outcome.has_value()) {
        return shared::ToolResult::fail(
            "Parent agent interrupted while waiting for subagent.");
      }
      if (isRetryableWaitOutcome(*outcome) && i + 1 < routes.size()) {
        continue;
      }
      return buildWaitResult(reusableAgentId, route, *outcome, i > 0);
    }

    try {
      reusableAgentId = Engine::instance().summonAgent(
          threadId, persona, delegatedTask, true,
          ctx.agent.getContext().identity.id, normalizedInput.name,
          normalizedInput.title, reusableAgentId, route.providerId,
          route.modelId, route.variantName, {}, summonOverrides);
      agentExists = true;
    } catch (const std::exception &) {
      if (i + 1 < routes.size()) {
        continue;
      }
      return shared::ToolResult::fail(
          "Failed to summon subagent on all configured routes.");
    }

    if (isExecutorRole(workRole) && normalizedInput.plan_id.has_value() &&
        normalizedInput.chunk_id.has_value()) {
      persistExecutorDispatch(threadId, *normalizedInput.plan_id,
                              *normalizedInput.chunk_id,
                              reusableAgentId);
    }

    if (input.async) {
      auto immediateOutcome =
          waitForOutcomeWithTimeout(reusableAgentId, kAsyncOutcomeProbeWindow);
      if (immediateOutcome.has_value()) {
        if (isRetryableWaitOutcome(*immediateOutcome) && i + 1 < routes.size()) {
          // Reset agentExists so fallback route spawns a fresh agent
          agentExists = false;
          continue;
        }
        if (immediateOutcome->kind == AgentOutcome::Kind::Cancelled) {
          return buildWaitResult(reusableAgentId, route, *immediateOutcome,
                                 i > 0);
        }
      }
      rapidjson::Document d;
      d.SetObject();
      auto &a = d.GetAllocator();
      d.AddMember("agentId", rapidjson::Value(reusableAgentId.c_str(), a).Move(), a);
      d.AddMember("status", "spawned", a);
      appendRoutingMetadata(d, route, attemptedCategories, i > 0);
      return shared::ToolResult::ok(d);
    }

    auto outcome = waitForOutcome(reusableAgentId);
    if (!outcome.has_value()) {
      return shared::ToolResult::fail(
          "Parent agent interrupted while waiting for subagent.");
    }
    if (isRetryableWaitOutcome(*outcome) && i + 1 < routes.size()) {
      // Reset agentExists so fallback route spawns a fresh agent
      agentExists = false;
      continue;
    }
    return buildWaitResult(reusableAgentId, route, *outcome, i > 0);
  }

  return shared::ToolResult::fail(
      "Subagent run failed or returned no usable summary on all routes.");
}

} // namespace firmius::core

#include "tools/DelegateTool.hpp"

#include "AgentRegistry.hpp"
#include "ConfigLoader.hpp"
#include "Engine.hpp"
#include "Events.hpp"
#include "Serialization.hpp"
#include "agents/PurposeLoader.hpp"
#include "artifacts/ReferenceExpansion.hpp"
#include "persistence/ThreadManager.hpp"
#include "tools/WorkSupport.hpp"
#include "utils/StringUtil.hpp"
#include <chrono>
#include <filesystem>
#include <optional>
#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>
#include <sstream>
#include <unordered_set>

namespace firmius::core {
using namespace firmius::shared;

namespace {

PurposeWorkRole purposeRoleForPersona(const std::string &persona) {
  PurposeWorkRole role = PurposeLoader::resolveWorkRole(persona);
  if (role != PurposeWorkRole::Unknown) return role;
  const std::string lowered = shared::StringUtil::toLower(shared::StringUtil::trim(persona));
  if (lowered == "executor") return PurposeWorkRole::Executor;
  if (lowered == "auditor") return PurposeWorkRole::Auditor;
  if (lowered == "worker") return PurposeWorkRole::Worker;
  if (lowered == "scout") return PurposeWorkRole::Scout;
  if (lowered == "dreamer" || lowered == "loom") return PurposeWorkRole::Auditor;
  return role;
}

std::optional<std::string> legacyPersonaSuggestion(const std::string &persona) {
  const std::string lowered = shared::StringUtil::toLower(shared::StringUtil::trim(persona));
  if (lowered == "implementer") return "legacy role 'implementer'; use 'executor'";
  if (lowered == "researcher") return "legacy role 'researcher'; use 'scout'";
  return std::nullopt;
}

bool isExecutorRole(PurposeWorkRole role) { return role == PurposeWorkRole::Executor; }
bool isAuditorRole(PurposeWorkRole role) { return role == PurposeWorkRole::Auditor; }
bool isWorkerLikeRole(PurposeWorkRole role) { return role == PurposeWorkRole::Worker || role == PurposeWorkRole::Scout; }

std::optional<std::string> normalizeOptionalString(const std::optional<std::string> &value) {
  if (!value.has_value()) return std::nullopt;
  const std::string trimmed = shared::StringUtil::trim(*value);
  if (trimmed.empty()) return std::nullopt;
  return trimmed;
}

bool callerMayUseDreamMode(const shared::ToolContext &ctx) {
  const std::string lowered = shared::StringUtil::toLower(shared::StringUtil::trim(ctx.agent.getContext().config.personaName));
  return lowered == "aster" || lowered == "fast" || lowered == "harbor" || lowered == "lead" || lowered == "hotrun";
}

bool isRetryableWaitOutcome(const AgentOutcome &outcome) {
  return outcome.kind == AgentOutcome::Kind::NoSummary || outcome.kind == AgentOutcome::Kind::Failed;
}

std::string normalizeRouteToken(const std::string& value) { return shared::StringUtil::toLower(shared::StringUtil::trim(value)); }

bool userExplicitlyRequestedCategory(const shared::ToolContext& ctx, const std::string& category) {
  const auto& agentContext = ctx.agent.getContext();
  if (!agentContext.history) return false;
  const std::string normalizedCategory = shared::StringUtil::toLower(shared::StringUtil::trim(category));
  if (normalizedCategory.empty()) return false;
  for (auto turnIt = agentContext.history->turns.rbegin(); turnIt != agentContext.history->turns.rend(); ++turnIt) {
    for (auto msgIt = turnIt->messages.rbegin(); msgIt != turnIt->messages.rend(); ++msgIt) {
      if (msgIt->role != shared::Role::User) continue;
      for (const auto& part : msgIt->content) {
        if (const auto* txt = std::get_if<shared::TextContent>(&part)) {
          const std::string lowered = normalizeRouteToken(txt->text);
          if (lowered.find(normalizedCategory) != std::string::npos) return true;
        }
      }
    }
  }
  return false;
}

const shared::WorkChunk &findChunk(const shared::Plan &plan, const std::string &chunkId) { return work::requireChunk(plan, chunkId); }
shared::Plan loadPlan(const std::string &threadId, const std::string &planId) {
  ThreadManager tm(ThreadManager::defaultBasePath());
  return tm.getPlan(threadId, planId);
}

std::optional<shared::Plan> loadRequestedOrActivePlan(const std::string &threadId, const std::optional<std::string> &requestedPlanId) {
  ThreadManager tm(ThreadManager::defaultBasePath());
  if (requestedPlanId.has_value() && !requestedPlanId->empty()) return tm.getPlan(threadId, *requestedPlanId);
  const auto metadata = tm.getMetadata(threadId);
  if (!metadata.activePlanId.empty()) return tm.getPlan(threadId, metadata.activePlanId);
  return std::nullopt;
}

std::string buildExecutorTask(const shared::Plan &plan, const shared::WorkChunk &chunk, const std::string &task) {
  std::ostringstream prompt;
  prompt << "You are the executor responsible for exactly one assigned work chunk.\n\nChunk Ownership Contract\n- You own exactly this chunk: " << chunk.id << "\n- You do not own the whole plan.\n- Do not take ownership of any other chunk.\n- Stay inside this chunk's scope. Do not silently repair sibling chunks or broader architecture.\n- If you discover an upstream, downstream, or cross-cutting problem outside this chunk, report it explicitly instead of claiming you fixed the plan.\n- You may delegate only bounded subtasks to worker/scout helpers one level deep.\n\nPlan Context\nPlan Title: " << plan.title << "\nPlan Objective: " << plan.objective << "\nPlan Strategy Summary: " << plan.strategy << "\n\nAssigned Chunk\nChunk ID: " << chunk.id << "\nChunk Title: " << chunk.title << "\nChunk Goal: " << chunk.goal << "\nChunk Context: " << chunk.context << "\nChunk Constraints: " << chunk.constraints << "\nChunk Completion: " << chunk.completion << "\n";
  if (!chunk.filesToRead.empty()) { prompt << "Files To Read: "; for (size_t i = 0; i < chunk.filesToRead.size(); ++i) { if (i > 0) prompt << ", "; prompt << chunk.filesToRead[i]; } prompt << "\n"; }
  if (!chunk.filesToTouch.empty()) { prompt << "Files To Touch: "; for (size_t i = 0; i < chunk.filesToTouch.size(); ++i) { if (i > 0) prompt << ", "; prompt << chunk.filesToTouch[i]; } prompt << "\n"; }
  if (!chunk.cwd.empty()) prompt << "Working Directory: " << chunk.cwd << "\n";
  if (!chunk.verificationCondition.empty()) prompt << "Verification Condition: " << chunk.verificationCondition << "\n";
  if (!chunk.handoffNotes.empty()) prompt << "Handoff Notes: " << chunk.handoffNotes << "\n";
  if (!chunk.tasks.empty()) {
    prompt << "\nChunk Tasks\nThis chunk contains " << chunk.tasks.size() << " internal tasks. Use these to structure your execution or delegate to workers:\n";
    for (const auto &t : chunk.tasks) {
      std::string statusLabel;
      switch (t.status) {
        case shared::WorkChunkStatus::Ready: statusLabel = "Ready"; break;
        case shared::WorkChunkStatus::InProgress: statusLabel = "InProgress"; break;
        case shared::WorkChunkStatus::Implemented: statusLabel = "Implemented"; break;
        case shared::WorkChunkStatus::Verifying: statusLabel = "Verifying"; break;
        case shared::WorkChunkStatus::Done: statusLabel = "Done"; break;
        case shared::WorkChunkStatus::Blocked: statusLabel = "Blocked"; break;
        case shared::WorkChunkStatus::Failed: statusLabel = "Failed"; break;
        case shared::WorkChunkStatus::Cancelled: statusLabel = "Cancelled"; break;
      }
      prompt << "- [" << statusLabel << "] " << t.title;
      if (!t.goal.empty()) prompt << ": " << t.goal;
      if (!t.notes.empty()) prompt << " (Note: " << t.notes << ")";
      if (!t.verificationCondition.empty()) prompt << " (Verify: " << t.verificationCondition << ")";
      prompt << "\n";
    }
  }
  prompt << "\nExecution Discipline\n- Reread the exact files and anchors you touch before editing.\n- If the target directory or files do not exist yet, that is not a blocker for greenfield chunk work; create the first scoped files directly with Edit content.\n- If an anchor or local context is stale, reread and repair it before editing; do not guess.\n- Do not broaden the task because a nearby cleanup looks tempting.\n- Do not claim completion, verification, or review without evidence.\n- Only the lead accepts work and marks a chunk Done after review; your terminal success state is normally Implemented.\n\nVerification Expectations\n- Run the concrete verification needed for this chunk. Prefer the narrowest checks that still produce real evidence.\n- Your report must name the verification commands or tests you ran and the outcome.\n- If verification is blocked or incomplete, say exactly why. Do not write 'looks correct' or equivalent guesswork.\n\nExecution State Reporting\n- If you report chunk progress with Work.updateChunk, use plan_id=\"" << plan.id << "\" and chunk_id=\"" << chunk.id << "\".\n- The only chunk fields you may write are: status, attempt_count, result_summary.\n- Valid chunk_update payload pattern: {\\\"plan_id\\\":\\\"" << plan.id << "\\\",\\\"chunk_id\\\":\\\"" << chunk.id << "\\\",\\\"status\\\":\\\"Implemented\\\",\\\"attempt_count\\\":1,\\\"result_summary\\\":\\\"implemented scoped changes; verified with focused evidence\\\"}.\n- Do not send title, goal, context, constraints, completion, depends_on, assigned_agent_id, or review_summary through updateChunk.\n- Do not send title, goal, context, constraints, completion, depends_on, assigned_agent_id, or review_summary through chunk_update.\n- Any design, review, dependency, or assignment fields in chunk_update will be rejected by runtime authority checks.\n- Use Implemented when the chunk changes are complete as far as you can take them with evidence.\n- Use Blocked or Failed when you hit a real blocker; say what blocked you.\n- Do not mark the chunk Done yourself.\n- Report back in a compact structure the lead can review quickly:\n  Changed: <files/behavior>\n  Verified: <command/test and result>\n  Blockers/Risks: <none or concrete issue>\n\n" << work::buildExecutorLockDoctrine() << "\n";
  if (!task.empty()) prompt << "\nLead Notes\n" << task << "\n";
  return prompt.str();
}

std::string buildWorkerTask(const std::string &task, const std::optional<shared::WorkTask> &workTask) {
  std::ostringstream prompt;
  prompt << "You are a worker helper supporting your parent executor on a bounded subtask.\n\nBoundaries\n- You do not own a plan chunk.\n- You are not responsible for the whole plan.\n- Complete only the bounded subtask below and return useful results to the executor.\n\n";
  if (workTask.has_value()) {
    prompt << "Assigned Task\nTask ID: " << workTask->id << "\nTask Title: " << workTask->title << "\nTask Goal: " << workTask->goal << "\n";
    if (!workTask->notes.empty()) prompt << "Task Notes: " << workTask->notes << "\n";
    if (!workTask->verificationCondition.empty()) prompt << "Verification: " << workTask->verificationCondition << "\n";
    prompt << "\n";
  }
  prompt << "Subtask\n" << task << "\n\n" << work::buildWorkerLockDoctrine() << "\n";
  return prompt.str();
}

std::string buildAuditorTask(const shared::Plan &plan, const shared::WorkChunk &chunk, const std::string &task) {
  std::ostringstream prompt;
  prompt << "You are the auditor responsible for evidence-backed review of a single work chunk.\n\nAuditor Contract\n- You do NOT own execution of this chunk.\n- Verify implementation and review evidence; do not implement.\n- Report a clear verdict with concrete evidence or identified gaps.\n- If evidence is missing or incomplete, say exactly what is missing.\n\nPlan Context\nPlan Title: " << plan.title << "\nPlan Objective: " << plan.objective << "\nPlan Strategy Summary: " << plan.strategy << "\n\nAssigned Chunk\nChunk ID: " << chunk.id << "\nChunk Title: " << chunk.title << "\nChunk Goal: " << chunk.goal << "\nChunk Context: " << chunk.context << "\nChunk Constraints: " << chunk.constraints << "\nChunk Completion: " << chunk.completion << "\nChunk Status: " << work::chunkStatusToString(chunk.status) << "\n";
  if (!chunk.assignedAgentId.empty()) prompt << "Assigned Executor: " << chunk.assignedAgentId << "\n";
  if (!chunk.resultSummary.empty()) prompt << "Result Summary: " << chunk.resultSummary << "\n";
  if (!chunk.reviewSummary.empty()) prompt << "Review Summary: " << chunk.reviewSummary << "\n";
  if (!chunk.filesToRead.empty()) { prompt << "Files To Read: "; for (size_t i = 0; i < chunk.filesToRead.size(); ++i) { if (i > 0) prompt << ", "; prompt << chunk.filesToRead[i]; } prompt << "\n"; }
  if (!chunk.filesToTouch.empty()) { prompt << "Files To Touch: "; for (size_t i = 0; i < chunk.filesToTouch.size(); ++i) { if (i > 0) prompt << ", "; prompt << chunk.filesToTouch[i]; } prompt << "\n"; }
  if (!chunk.cwd.empty()) prompt << "Working Directory: " << chunk.cwd << "\n";
  if (!chunk.verificationCondition.empty()) prompt << "Verification Condition: " << chunk.verificationCondition << "\n";
  if (!chunk.handoffNotes.empty()) prompt << "Handoff Notes: " << chunk.handoffNotes << "\n";
  if (!chunk.tasks.empty()) {
    prompt << "\nChunk Tasks\nThis chunk contains " << chunk.tasks.size() << " internal tasks. Use these to anchor review evidence:\n";
    for (const auto &t : chunk.tasks) {
      std::string statusLabel;
      switch (t.status) {
        case shared::WorkChunkStatus::Ready: statusLabel = "Ready"; break;
        case shared::WorkChunkStatus::InProgress: statusLabel = "InProgress"; break;
        case shared::WorkChunkStatus::Implemented: statusLabel = "Implemented"; break;
        case shared::WorkChunkStatus::Verifying: statusLabel = "Verifying"; break;
        case shared::WorkChunkStatus::Done: statusLabel = "Done"; break;
        case shared::WorkChunkStatus::Blocked: statusLabel = "Blocked"; break;
        case shared::WorkChunkStatus::Failed: statusLabel = "Failed"; break;
        case shared::WorkChunkStatus::Cancelled: statusLabel = "Cancelled"; break;
      }
      prompt << "- [" << statusLabel << "] " << t.title;
      if (!t.goal.empty()) prompt << ": " << t.goal;
      if (!t.notes.empty()) prompt << " (Note: " << t.notes << ")";
      if (!t.verificationCondition.empty()) prompt << " (Verify: " << t.verificationCondition << ")";
      prompt << "\n";
    }
  }
  prompt << "\nReview Discipline\n- Prefer direct evidence: tests, logs, file diffs, or runtime checks.\n- Do not accept claims without evidence.\n- Provide an explicit verdict: accept, reject, or needs more evidence.\n- Report in a compact structure the lead can act on:\n  Verdict: <accept/reject/needs-evidence>\n  Evidence: <commands/tests/files>\n  Gaps/Risks: <none or concrete issue>\n";
  if (!task.empty()) prompt << "\nLead Notes\n" << task << "\n";
  return prompt.str();
}

void ensureExecutorAssignmentAvailable(const std::string &threadId, const std::string &planId, const std::string &chunkId, const std::optional<std::string> &agentId) {
  ThreadManager tm(ThreadManager::defaultBasePath());
  const shared::Plan plan = tm.getPlan(threadId, planId);
  const shared::WorkChunk &chunk = findChunk(plan, chunkId);
  if (!chunk.assignedAgentId.empty() && (!agentId.has_value() || chunk.assignedAgentId != *agentId)) {
    throw std::runtime_error("Chunk '" + chunkId + "' is already owned by executor agent '" + chunk.assignedAgentId + "'");
  }
  if (!agentId.has_value() || agentId->empty()) return;
  for (const auto &candidatePlan : tm.listPlans(threadId)) {
    for (const auto &candidateChunk : candidatePlan.chunks) {
      if (candidateChunk.assignedAgentId != *agentId) continue;
      if (candidatePlan.id == planId && candidateChunk.id == chunkId) continue;
      throw std::runtime_error("Executor agent '" + *agentId + "' already owns chunk '" + candidateChunk.id + "'");
    }
  }
}

void ensureExecutorChunkReadyForDispatch(const std::string &threadId, const std::string &planId, const std::string &chunkId) {
  ThreadManager tm(ThreadManager::defaultBasePath());
  shared::Plan plan = tm.getPlan(threadId, planId);
  auto &chunk = work::requireChunk(plan, chunkId);
  const shared::WorkChunk originalChunk = chunk;
  if (work::blockChunkIfDependenciesIncomplete(plan, chunk)) {
    chunk.updatedAt = work::nowEpochMs();
    tm.updatePlan(threadId, plan);
    work::emitWorkEvent(shared::ChunkUpdated{threadId, plan.id, chunk});
    work::emitWorkEvent(shared::ChunkStatusChanged{threadId, plan.id, chunk.id, originalChunk.status, chunk.status, chunk});
  }
  work::requireChunkReadyForExecution(plan, chunk, "dispatch");
}

void persistExecutorDispatch(const std::string &threadId, const std::string &planId, const std::string &chunkId, const std::string &agentId) {
  ThreadManager tm(ThreadManager::defaultBasePath());
  shared::Plan plan = tm.getPlan(threadId, planId);
  auto &chunk = work::requireChunk(plan, chunkId);
  const shared::WorkChunk originalChunk = chunk;
  work::validateExecutorAssignmentInvariant(tm, threadId, plan.id, chunk.id, agentId);
  chunk.assignedAgentId = agentId;
  if (chunk.status == shared::WorkChunkStatus::Ready) chunk.status = shared::WorkChunkStatus::InProgress;
  chunk.updatedAt = work::nowEpochMs();
  tm.updatePlan(threadId, plan);
  work::emitWorkEvent(shared::ChunkUpdated{threadId, plan.id, chunk});
  if (originalChunk.assignedAgentId != chunk.assignedAgentId) work::emitWorkEvent(shared::ChunkAssigned{threadId, plan.id, chunk.id, chunk.assignedAgentId, chunk});
  if (originalChunk.status != chunk.status) work::emitWorkEvent(shared::ChunkStatusChanged{threadId, plan.id, chunk.id, originalChunk.status, chunk.status, chunk});
}

std::string buildDelegationTask(const std::string &/*persona*/, const std::string &task, const std::optional<std::string> &plan_id, const std::optional<std::string> &chunk_id, const std::optional<std::string> &task_id, const std::string &threadId, PurposeWorkRole role) {
  if (isExecutorRole(role) && plan_id.has_value() && chunk_id.has_value()) {
    const shared::Plan plan = loadPlan(threadId, *plan_id);
    const shared::WorkChunk &chunk = findChunk(plan, *chunk_id);
    return buildExecutorTask(plan, chunk, task);
  }
  if (isAuditorRole(role) && plan_id.has_value() && chunk_id.has_value()) {
    const shared::Plan plan = loadPlan(threadId, *plan_id);
    const shared::WorkChunk &chunk = findChunk(plan, *chunk_id);
    return buildAuditorTask(plan, chunk, task);
  }
  if (isWorkerLikeRole(role) && task_id.has_value() && !task_id->empty() && plan_id.has_value() && chunk_id.has_value()) {
    const shared::Plan plan = loadPlan(threadId, *plan_id);
    const shared::WorkChunk &chunk = findChunk(plan, *chunk_id);
    auto taskIt = std::find_if(chunk.tasks.begin(), chunk.tasks.end(), [&](const shared::WorkTask &t) { return t.id == *task_id; });
    if (taskIt != chunk.tasks.end()) return buildWorkerTask(task, *taskIt);
  }
  if (isWorkerLikeRole(role)) return buildWorkerTask(task, std::nullopt);
  return task;
}

std::string buildDreamerTask(const std::string &task, const std::optional<std::string> &plan_id, const std::string &threadId, const shared::ToolContext &ctx, const std::string &memoryRoot) {
  std::ostringstream prompt;
  prompt << "You are being summoned in restricted dream mode by a lead agent.\n\nDream Sandbox\n- Working directory: " << memoryRoot << "\n- Read/write only under that directory.\n- Do not modify the project repository itself.\n- Prefer USER.md, BEHAVIOR.md, and project-specific notes under projects/.\n\n";
  const auto &parentCtx = ctx.agent.getContext();
  prompt << "Lead Context\n- Parent persona: " << parentCtx.config.personaName << "\n- Source workspace: " << parentCtx.environment.cwd << "\n- Thread ID: " << threadId << "\n\n";
  if (auto plan = loadRequestedOrActivePlan(threadId, plan_id); plan.has_value()) {
    prompt << "Plan Context\n- Title: " << plan->title << "\n- Objective: " << plan->objective << "\n- Strategy: " << plan->strategy << "\n- Chunks:\n";
    for (const auto &chunk : plan->chunks) prompt << "  - " << chunk.title << " (" << chunk.id << ")\n";
    prompt << "\n";
  }
  prompt << "Lead Dream Request\n" << task << "\n";
  return prompt.str();
}

struct ResolvedRoute {
  std::string providerId;
  std::string modelId;
  std::string variantName;
  std::string categoryName;
  std::string warning;
};

ResolvedRoute resolveModelRoute(const std::string &persona, const std::optional<std::string>& explicitCategoryOverride = std::nullopt, const std::string& explicitCategoryWarning = "") {
  const auto &config = shared::ConfigLoader::instance().getConfig();
  auto useDefaultRoute = [&config]() { return ResolvedRoute{config.defaultProviderId, config.defaultModelId, config.defaultModelVariant, "", ""}; };
  auto findCategory = [&config](const std::string &name) -> const shared::ModelRouteCategory * {
    auto it = config.modelRouterCategories.find(name);
    return it == config.modelRouterCategories.end() ? nullptr : &it->second;
  };
  if (explicitCategoryOverride.has_value() && !explicitCategoryOverride->empty()) {
    if (const auto *category = findCategory(*explicitCategoryOverride)) {
      if (!category->models.empty()) {
        const auto &opt = category->models.front();
        return {opt.providerId, opt.modelId, opt.variantName, *explicitCategoryOverride, explicitCategoryWarning};
      }
    }
    auto route = useDefaultRoute();
    route.warning = "Category '" + *explicitCategoryOverride + "' not found; using default model route.";
    return route;
  }
  auto it_purpose = config.purposeRoutes.find(persona);
  if (it_purpose != config.purposeRoutes.end() && !it_purpose->second.empty()) {
    const std::string mapped_category = it_purpose->second;
    if (const auto *category = findCategory(mapped_category)) {
      if (!category->models.empty()) {
        const auto &opt = category->models.front();
        return {opt.providerId, opt.modelId, opt.variantName, mapped_category, ""};
      }
    }
    auto route = useDefaultRoute();
    route.warning = "Purpose route for '" + persona + "' points to missing category '" + mapped_category + "'; using default model route.";
    return route;
  }
  if (!config.defaultRouteCategory.empty()) {
    if (const auto *category = findCategory(config.defaultRouteCategory)) {
      if (!category->models.empty()) {
        const auto &opt = category->models.front();
        return {opt.providerId, opt.modelId, opt.variantName, config.defaultRouteCategory, ""};
      }
    }
  }
  return useDefaultRoute();
}

std::vector<ResolvedRoute> buildRouteCandidates(const std::string &persona, const std::optional<std::string>& explicitCategoryOverride = std::nullopt, const std::string& explicitCategoryWarning = "") {
  const auto &config = shared::ConfigLoader::instance().getConfig();
  std::vector<ResolvedRoute> routes;
  std::unordered_set<std::string> seen;
  auto pushUnique = [&](const ResolvedRoute &route) {
    std::string key = route.providerId + "|" + route.modelId + "|" + route.variantName + "|" + route.categoryName;
    if (seen.insert(key).second) routes.push_back(route);
  };
  ResolvedRoute primary = resolveModelRoute(persona, explicitCategoryOverride, "");
  if (!explicitCategoryWarning.empty()) primary.warning = explicitCategoryWarning + (primary.warning.empty() ? "" : " " + primary.warning);
  pushUnique(primary);
  if (!config.enableSubagentRouteFallback) return routes;
  std::vector<std::string> fallbackCategories = config.subagentRouteFallbackOrder;
  if (fallbackCategories.empty()) { for (const auto &[name, _] : config.modelRouterCategories) fallbackCategories.push_back(name); }
  for (const auto &name : fallbackCategories) {
    auto it = config.modelRouterCategories.find(name);
    if (it == config.modelRouterCategories.end() || it->second.models.empty()) continue;
    const auto &opt = it->second.models.front();
    pushUnique(ResolvedRoute{opt.providerId, opt.modelId, opt.variantName, name, ""});
  }
  return routes;
}

void appendRoutingMetadata(rapidjson::Document &d, const ResolvedRoute &route, const std::vector<std::string> &attemptedCategories, bool fallbackUsed) {
  auto &a = d.GetAllocator();
  if (!route.categoryName.empty()) d.AddMember("category", rapidjson::Value(route.categoryName.c_str(), a).Move(), a);
  rapidjson::Value attempted(rapidjson::kArrayType);
  for (const auto &category : attemptedCategories) attempted.PushBack(rapidjson::Value(category.c_str(), a).Move(), a);
  d.AddMember("attempted_categories", attempted, a);
  d.AddMember("fallback_used", fallbackUsed, a);
  if (!route.warning.empty()) d.AddMember("routing_warning", rapidjson::Value(route.warning.c_str(), a).Move(), a);
}

void appendOutcomeArtifacts(rapidjson::Document &d, const shared::AgentOutcome &outcome) {
  auto &a = d.GetAllocator();
  auto appendArray = [&](const char *key, const std::vector<shared::ThreadArtifactMetadata> &items) {
    rapidjson::Value array(rapidjson::kArrayType);
    for (const auto &artifact : items) {
      rapidjson::Document artifactDoc = shared::toJson(artifact);
      rapidjson::Value artifactValue; artifactValue.CopyFrom(artifactDoc, a);
      const std::string owner = artifact.ownerFriendlyName.empty() ? artifact.ownerAgentId : artifact.ownerFriendlyName;
      const std::string reference = "@artifact:" + owner + "/" + artifact.filename;
      artifactValue.AddMember("reference", rapidjson::Value(reference.c_str(), a).Move(), a);
      array.PushBack(artifactValue, a);
    }
    d.AddMember(rapidjson::Value(key, a).Move(), array, a);
  };
  appendArray("artifacts_created", outcome.artifacts_created);
  appendArray("artifacts_updated", outcome.artifacts_updated);
}

shared::ToolResult failWithStructuredData(const rapidjson::Document &d, const std::string &error) {
  shared::ToolResult result = shared::ToolResult::fail(error);
  rapidjson::StringBuffer buffer;
  rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
  d.Accept(writer);
  result.data = buffer.GetString();
  return result;
}

std::size_t releaseOwnedChunksForAgent(const std::string &threadId, const std::string &agentId) {
  ThreadManager tm(ThreadManager::defaultBasePath());
  std::size_t released = 0;
  for (auto plan : tm.listPlans(threadId)) {
    bool changed = false;
    for (auto &chunk : plan.chunks) {
      if (chunk.assignedAgentId != agentId) continue;
      const auto originalChunk = chunk;
      chunk.assignedAgentId.clear();
      if (chunk.status == shared::WorkChunkStatus::InProgress) chunk.status = shared::WorkChunkStatus::Ready;
      chunk.updatedAt = work::nowEpochMs();
      changed = true; ++released;
      work::emitWorkEvent(shared::ChunkUpdated{threadId, plan.id, chunk});
      work::emitWorkEvent(shared::ChunkAssigned{threadId, plan.id, chunk.id, chunk.assignedAgentId, chunk});
      if (originalChunk.status != chunk.status) work::emitWorkEvent(shared::ChunkStatusChanged{threadId, plan.id, chunk.id, originalChunk.status, chunk.status, chunk});
    }
    if (changed) tm.updatePlan(threadId, plan);
  }
  return released;
}

shared::ToolResult executeSpawn(const rapidjson::Value &input, shared::ToolContext &ctx) {
  bool dream = false;
  if (input.HasMember("dream") && input["dream"].IsBool()) dream = input["dream"].GetBool();

  std::string persona;
  if (dream) {
    if (!callerMayUseDreamMode(ctx)) return shared::ToolResult::fail("dream summon mode is restricted to aster, fast, harbor, or lead agents");
    persona = "dreamer";
  } else if (input.HasMember("persona") && input["persona"].IsString()) {
    persona = shared::StringUtil::trim(std::string(input["persona"].GetString()));
  } else {
    return shared::ToolResult::fail("Missing required field: persona");
  }

  if (!PurposeLoader::isValid(persona)) {
    if (auto suggestion = legacyPersonaSuggestion(persona)) return shared::ToolResult::fail("Invalid persona: '" + persona + "'. " + *suggestion + ".");
    return shared::ToolResult::fail("Invalid persona: '" + persona + "'. Check available personas in base.md or prompts/ directory.");
  }

  std::string task;
  if (input.HasMember("task") && input["task"].IsString()) task = input["task"].GetString();
  else return shared::ToolResult::fail("Missing required field: task");

  std::string name;
  if (input.HasMember("name") && input["name"].IsString()) name = input["name"].GetString();
  else return shared::ToolResult::fail("Missing required field: name");

  std::string title;
  if (input.HasMember("title") && input["title"].IsString()) title = input["title"].GetString();
  else return shared::ToolResult::fail("Missing required field: title");

  std::optional<std::string> agent_id;
  if (input.HasMember("agent_id") && input["agent_id"].IsString()) agent_id = normalizeOptionalString(input["agent_id"].GetString());

  std::optional<std::string> plan_id;
  if (input.HasMember("plan_id") && input["plan_id"].IsString()) plan_id = normalizeOptionalString(input["plan_id"].GetString());

  std::optional<std::string> chunk_id;
  if (input.HasMember("chunk_id") && input["chunk_id"].IsString()) chunk_id = normalizeOptionalString(input["chunk_id"].GetString());

  std::optional<std::string> task_id;
  if (input.HasMember("task_id") && input["task_id"].IsString()) task_id = normalizeOptionalString(input["task_id"].GetString());

  std::optional<std::string> category;
  if (input.HasMember("category") && input["category"].IsString()) category = normalizeOptionalString(input["category"].GetString());

  bool isAsync = false;
  if (input.HasMember("async") && input["async"].IsBool()) isAsync = input["async"].GetBool();

  PurposeWorkRole workRole = purposeRoleForPersona(persona);
  std::string threadId = ctx.agent.getContext().history->threadId;

  if (plan_id.has_value() != chunk_id.has_value()) return shared::ToolResult::fail("plan_id and chunk_id must either both be provided or both be omitted");
  if (task_id.has_value() && !task_id->empty() && (!plan_id.has_value() || !chunk_id.has_value())) return shared::ToolResult::fail("task_id requires both plan_id and chunk_id to be provided");

  if (isExecutorRole(workRole) && plan_id.has_value() && chunk_id.has_value()) {
    try {
      ensureExecutorChunkReadyForDispatch(threadId, *plan_id, *chunk_id);
      ensureExecutorAssignmentAvailable(threadId, *plan_id, *chunk_id, agent_id);
    } catch (const std::exception &e) { return shared::ToolResult::fail(e.what()); }
  }

  if (isWorkerLikeRole(workRole) && task_id.has_value() && !task_id->empty() && plan_id.has_value() && chunk_id.has_value()) {
    try {
      ThreadManager tm(ThreadManager::defaultBasePath());
      shared::Plan plan = tm.getPlan(threadId, *plan_id);
      auto &chunk = work::requireChunk(plan, *chunk_id);
      auto taskIt = std::find_if(chunk.tasks.begin(), chunk.tasks.end(), [&](const shared::WorkTask &t) { return t.id == *task_id; });
      if (taskIt == chunk.tasks.end()) return shared::ToolResult::fail("Task '" + *task_id + "' not found in chunk '" + *chunk_id + "'");
      if (!taskIt->assignedWorkerId.empty() && (!agent_id.has_value() || taskIt->assignedWorkerId != *agent_id)) return shared::ToolResult::fail("Task '" + *task_id + "' is already assigned to worker '" + taskIt->assignedWorkerId + "'");
      if (agent_id.has_value() && !agent_id->empty()) {
        taskIt->assignedWorkerId = *agent_id;
        taskIt->updatedAt = work::nowEpochMs();
        chunk.updatedAt = taskIt->updatedAt;
        tm.updatePlan(threadId, plan);
      }
    } catch (const std::exception &e) { return shared::ToolResult::fail(e.what()); }
  }

  std::string delegatedTask;
  std::optional<SummonAgentOverrides> summonOverrides;
  try {
    std::string builtTask;
    if (dream) {
      const std::string home = std::getenv("HOME") ? std::getenv("HOME") : "/root";
      const std::string memoryRoot = home + "/.firmius/user";
      std::filesystem::create_directories(memoryRoot);
      builtTask = buildDreamerTask(task, plan_id, threadId, ctx, memoryRoot);
      summonOverrides = SummonAgentOverrides{.cwdOverride = memoryRoot, .allowedPathsOverride = std::vector<std::string>{memoryRoot, memoryRoot + "/**"}};
    } else {
      builtTask = buildDelegationTask(persona, task, plan_id, chunk_id, task_id, threadId, workRole);
    }
    const std::string cwd = ctx.agent.getContext().environment.cwd;
    delegatedTask = firmius::core::artifacts::expandInboundReferences(threadId, cwd, builtTask);
  } catch (const std::exception &e) { return shared::ToolResult::fail("Reference expansion failed: " + std::string(e.what())); }

  auto existingAgents = AgentRegistry::instance().listAll();
  for (const auto &aid : existingAgents) {
    auto ag = AgentRegistry::instance().getAgent(aid);
    if (ag && ag->getContext().identity.friendlyName == name && (!agent_id.has_value() || aid != *agent_id)) return shared::ToolResult::fail("Agent name '" + name + "' already exists in this thread");
  }

  std::optional<std::string> explicitCategoryOverride;
  std::string explicitCategoryWarning;
  if (category.has_value() && !category->empty()) {
    if (userExplicitlyRequestedCategory(ctx, *category)) explicitCategoryOverride = category;
    else explicitCategoryWarning = "Ignored explicit category '" + *category + "' because only user-specified route-category overrides are honored; using configured purpose/default routing.";
  }

  const std::vector<ResolvedRoute> routes = buildRouteCandidates(persona, explicitCategoryOverride, explicitCategoryWarning);
  std::vector<std::string> attemptedCategories;
  const bool isRetaskingExistingAgent = agent_id.has_value() && !agent_id->empty();
  std::string reusableAgentId = isRetaskingExistingAgent ? *agent_id : shared::StringUtil::generateUuid();
  bool agentExists = isRetaskingExistingAgent;

  auto waitForOutcome = [&](const std::string &aid) -> std::optional<AgentOutcome> {
    while (true) {
      auto outcome = Engine::instance().waitForAgentOutcome(aid, std::chrono::milliseconds(20));
      if (outcome.has_value()) return outcome;
      if (ctx.cancelRequested()) { Engine::instance().cancelAgent(aid); return AgentOutcome{AgentOutcome::Kind::Cancelled, "Cancelled by parent."}; }
    }
  };

  auto waitForOutcomeWithTimeout = [&](const std::string &aid, std::chrono::milliseconds timeout) -> std::optional<AgentOutcome> {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
      auto outcome = Engine::instance().peekAgentOutcome(aid, std::chrono::milliseconds(20));
      if (outcome.has_value()) return outcome;
      if (ctx.cancelRequested()) { Engine::instance().cancelAgent(aid); return AgentOutcome{AgentOutcome::Kind::Cancelled, "Cancelled by parent."}; }
    }
    return std::nullopt;
  };

  auto buildWaitResult = [&](const std::string &aid, const ResolvedRoute &route, const AgentOutcome &outcome, bool fallbackUsed) -> shared::ToolResult {
    rapidjson::Document d; d.SetObject(); auto &a = d.GetAllocator();
    d.AddMember("agentId", rapidjson::Value(aid.c_str(), a).Move(), a);
    appendRoutingMetadata(d, route, attemptedCategories, fallbackUsed);
    appendOutcomeArtifacts(d, outcome);
    if (outcome.kind == AgentOutcome::Kind::Cancelled) {
      d.AddMember("status", "cancelled", a); d.AddMember("result", rapidjson::Value(outcome.text.c_str(), a).Move(), a);
      return shared::ToolResult::ok(d);
    }
    if (outcome.kind == AgentOutcome::Kind::Failed) {
      d.AddMember("status", "failed", a); d.AddMember("error", rapidjson::Value(outcome.text.c_str(), a).Move(), a);
      return failWithStructuredData(d, outcome.text);
    }
    if (outcome.kind == AgentOutcome::Kind::NoSummary) {
      d.AddMember("status", "completed_no_summary", a); d.AddMember("result", rapidjson::Value(outcome.text.c_str(), a).Move(), a);
      return shared::ToolResult::ok(d);
    }
    d.AddMember("status", "completed", a); d.AddMember("result", rapidjson::Value(outcome.text.c_str(), a).Move(), a);
    return shared::ToolResult::ok(d);
  };

  for (std::size_t i = 0; i < routes.size(); ++i) {
    const auto &route = routes[i];
    attemptedCategories.push_back(route.categoryName.empty() ? "default" : route.categoryName);
    if (agentExists) {
      auto ag = AgentRegistry::instance().getAgent(reusableAgentId);
      if (!ag) return shared::ToolResult::fail("Agent not found: " + reusableAgentId);
      if (ag->getContext().state.currentStatus != AgentStatus::Idle) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (ag->getContext().state.currentStatus != AgentStatus::Idle && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(std::chrono::milliseconds(50));
        if (ag->getContext().state.currentStatus != AgentStatus::Idle) return shared::ToolResult::fail("Agent is busy (status: " + std::to_string(static_cast<int>(ag->getContext().state.currentStatus)) + ")");
      }
      try {
        Engine::instance().switchAgentModel(reusableAgentId, route.providerId, route.modelId, route.variantName);
        Engine::instance().executeTask(reusableAgentId, delegatedTask);
      } catch (const std::exception &) { if (i + 1 < routes.size()) continue; return shared::ToolResult::fail("Failed to launch subagent run on all configured routes."); }
      if (isExecutorRole(workRole) && plan_id.has_value() && chunk_id.has_value()) persistExecutorDispatch(threadId, *plan_id, *chunk_id, reusableAgentId);
      if (isAsync) {
        auto immediate = waitForOutcomeWithTimeout(reusableAgentId, std::chrono::milliseconds(250));
        if (immediate.has_value()) {
          if (isRetryableWaitOutcome(*immediate) && i + 1 < routes.size()) continue;
          if (immediate->kind == AgentOutcome::Kind::Cancelled) return buildWaitResult(reusableAgentId, route, *immediate, i > 0);
        }
        rapidjson::Document d; d.SetObject(); auto &a = d.GetAllocator();
        d.AddMember("agentId", rapidjson::Value(reusableAgentId.c_str(), a).Move(), a);
        d.AddMember("status", "re-tasked", a);
        appendRoutingMetadata(d, route, attemptedCategories, i > 0);
        return shared::ToolResult::ok(d);
      }
      auto outcome = waitForOutcome(reusableAgentId);
      if (!outcome.has_value()) return shared::ToolResult::fail("Parent agent interrupted while waiting for subagent.");
      if (isRetryableWaitOutcome(*outcome) && i + 1 < routes.size()) continue;
      return buildWaitResult(reusableAgentId, route, *outcome, i > 0);
    }
    try {
      reusableAgentId = Engine::instance().summonAgent(threadId, persona, delegatedTask, true, ctx.agent.getContext().identity.id, name, title, reusableAgentId, route.providerId, route.modelId, route.variantName, {}, summonOverrides);
      agentExists = true;
    } catch (const std::exception &) { if (i + 1 < routes.size()) continue; return shared::ToolResult::fail("Failed to summon subagent on all configured routes."); }
    if (isExecutorRole(workRole) && plan_id.has_value() && chunk_id.has_value()) persistExecutorDispatch(threadId, *plan_id, *chunk_id, reusableAgentId);
    if (isAsync) {
      auto immediate = waitForOutcomeWithTimeout(reusableAgentId, std::chrono::milliseconds(250));
      if (immediate.has_value()) {
        if (isRetryableWaitOutcome(*immediate) && i + 1 < routes.size()) { agentExists = false; continue; }
        if (immediate->kind == AgentOutcome::Kind::Cancelled) return buildWaitResult(reusableAgentId, route, *immediate, i > 0);
      }
      rapidjson::Document d; d.SetObject(); auto &a = d.GetAllocator();
      d.AddMember("agentId", rapidjson::Value(reusableAgentId.c_str(), a).Move(), a);
      d.AddMember("status", "spawned", a);
      appendRoutingMetadata(d, route, attemptedCategories, i > 0);
      return shared::ToolResult::ok(d);
    }
    auto outcome = waitForOutcome(reusableAgentId);
    if (!outcome.has_value()) return shared::ToolResult::fail("Parent agent interrupted while waiting for subagent.");
    if (isRetryableWaitOutcome(*outcome) && i + 1 < routes.size()) { agentExists = false; continue; }
    return buildWaitResult(reusableAgentId, route, *outcome, i > 0);
  }
  return shared::ToolResult::fail("Subagent run failed or returned no usable summary on all routes.");
}

shared::ToolResult executeWait(const rapidjson::Value &input, shared::ToolContext &ctx) {
  std::string agent_id;
  if (input.HasMember("agent_id") && input["agent_id"].IsString()) agent_id = input["agent_id"].GetString();
  else return shared::ToolResult::fail("Missing required field: agent_id");

  std::optional<AgentOutcome> outcome;
  while (true) {
    outcome = Engine::instance().waitForAgentOutcome(agent_id, std::chrono::milliseconds(25));
    if (outcome.has_value()) break;
    if (ctx.cancelRequested()) { Engine::instance().cancelAgent(agent_id); return shared::ToolResult::fail("Parent agent interrupted while waiting for subagent."); }
  }
  rapidjson::Document d; d.SetObject(); auto& a = d.GetAllocator();
  d.AddMember("agentId", rapidjson::Value(agent_id.c_str(), a).Move(), a);
  {
    auto agent = AgentRegistry::instance().getAgent(agent_id);
    const std::string friendlyName = agent && !agent->getContext().identity.friendlyName.empty() ? agent->getContext().identity.friendlyName : "";
    d.AddMember("friendlyName", rapidjson::Value(friendlyName.c_str(), a).Move(), a);
  }
  appendOutcomeArtifacts(d, *outcome);
  if (outcome->kind == AgentOutcome::Kind::Cancelled) {
    d.AddMember("status", "cancelled", a); d.AddMember("result", rapidjson::Value(outcome->text.c_str(), a).Move(), a);
    return shared::ToolResult::ok(d);
  }
  if (outcome->kind == AgentOutcome::Kind::Failed) {
    d.AddMember("status", "failed", a); d.AddMember("error", rapidjson::Value(outcome->text.c_str(), a).Move(), a);
    return failWithStructuredData(d, outcome->text);
  }
  if (outcome->kind == AgentOutcome::Kind::NoSummary) {
    d.AddMember("status", "completed_no_summary", a); d.AddMember("result", rapidjson::Value(outcome->text.c_str(), a).Move(), a);
    return shared::ToolResult::ok(d);
  }
  d.AddMember("status", "completed", a); d.AddMember("result", rapidjson::Value(outcome->text.c_str(), a).Move(), a);
  return shared::ToolResult::ok(d);
}

shared::ToolResult executeStop(const rapidjson::Value &input, shared::ToolContext &ctx) {
  std::string agent_id;
  if (input.HasMember("agent_id") && input["agent_id"].IsString()) agent_id = input["agent_id"].GetString();
  else return shared::ToolResult::fail("Missing required field: agent_id");

  const std::string threadId = work::requireCurrentThreadId(ctx);
  Engine::instance().terminateAgent(agent_id);
  const std::size_t releasedChunks = releaseOwnedChunksForAgent(threadId, agent_id);
  rapidjson::Document d; d.SetObject(); auto& a = d.GetAllocator();
  d.AddMember("agent_id", rapidjson::Value(agent_id.c_str(), a).Move(), a);
  d.AddMember("status", "terminated", a);
  d.AddMember("released_chunk_count", static_cast<uint64_t>(releasedChunks), a);
  return shared::ToolResult::ok(d);
}

} // namespace

shared::ToolMetadata DelegateTool::getMetadata() const {
  return {"Delegate", "Subagent operations. Use action Spawn, Wait, or Stop.", shared::ToolScope::Delegation};
}

std::shared_ptr<shared::JSONSchema> DelegateTool::getSchema() const {
  return shared::zObject({
      {"action", shared::zEnum({"Spawn", "Wait", "Stop"})->describe("Delegation operation to execute")},
      {"agent_id", shared::zString()->setOptional()},
      {"persona", shared::zString()->setOptional()},
      {"task", shared::zString()->setOptional()},
      {"title", shared::zString()->setOptional()},
      {"name", shared::zString()->setOptional()},
      {"plan_id", shared::zString()->setOptional()},
      {"chunk_id", shared::zString()->setOptional()},
      {"task_id", shared::zString()->setOptional()},
      {"category", shared::zString()->setOptional()},
      {"async", shared::zBoolean()->setOptional()},
      {"dream", shared::zBoolean()->setOptional()},
  });
}

shared::ToolResult DelegateTool::execute(const rapidjson::Value &input, shared::ToolContext &ctx) {
  if (!input.IsObject() || !input.HasMember("action") || !input["action"].IsString()) {
    return shared::ToolResult::fail("Delegate.action must be a string (Spawn, Wait, or Stop)");
  }
  const std::string action = input["action"].GetString();
  if (action == "Spawn") return executeSpawn(input, ctx);
  if (action == "Wait") return executeWait(input, ctx);
  if (action == "Stop") return executeStop(input, ctx);
  return shared::ToolResult::fail("Delegate.action must be Spawn, Wait, or Stop");
}

} // namespace firmius::core

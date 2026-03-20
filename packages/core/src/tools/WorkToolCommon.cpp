#include "tools/WorkToolCommon.hpp"
#include "IAgent.hpp"
#include "harness/Harness.hpp"
#include "utils/StringUtil.hpp"
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <set>

namespace firmius::core::worktools {

namespace {

std::runtime_error permissionError(const std::string &message) {
  return std::runtime_error("Work authority denied: " + message);
}

std::runtime_error roleError(const std::string &message) {
  return std::runtime_error("Invalid work role: " + message);
}

std::string scopeName(shared::ToolScope scope) {
  switch (scope) {
  case shared::ToolScope::FilesystemRead:
    return "FilesystemRead";
  case shared::ToolScope::FilesystemWrite:
    return "FilesystemWrite";
  case shared::ToolScope::Process:
    return "Process";
  case shared::ToolScope::Semantic:
    return "Semantic";
  case shared::ToolScope::Delegation:
    return "Delegation";
  case shared::ToolScope::Web:
    return "Web";
  case shared::ToolScope::Git:
    return "Git";
  case shared::ToolScope::PlanRead:
    return "PlanRead";
  case shared::ToolScope::PlanWrite:
    return "PlanWrite";
  case shared::ToolScope::ChunkRead:
    return "ChunkRead";
  case shared::ToolScope::ChunkWrite:
    return "ChunkWrite";
  case shared::ToolScope::ChunkAssign:
    return "ChunkAssign";
  case shared::ToolScope::ChunkReview:
    return "ChunkReview";
  }
  return "Unknown";
}

std::set<std::string> requestedChunkUpdateFields(const rapidjson::Value &input) {
  static const std::vector<std::string> kMutableFields = {
      "title",         "goal",          "context",      "constraints",
      "completion",    "planning_gate", "status",       "depends_on",
      "attempt_count", "result_summary", "review_summary", "assigned_agent_id",
      // V2 rich chunk spec fields
      "files_to_read", "files_to_touch", "cwd",
      "verification_condition", "handoff_notes"};

  std::set<std::string> fields;
  for (const auto &field : kMutableFields) {
    if (input.HasMember(field.c_str())) {
      fields.insert(field);
    }
  }
  return fields;
}

void requireScope(const shared::ToolContext &ctx, shared::ToolScope scope,
                  const std::string &action) {
  if (!hasScope(ctx, scope)) {
    throw permissionError(action + " requires scope " + scopeName(scope));
  }
}

void requireRole(const shared::ToolContext &ctx, WorkAgentRole expected,
                 const std::string &action) {
  const WorkAgentRole actual = roleForContext(ctx);
  if (actual != expected) {
    throw permissionError(action + " is restricted to " + roleName(expected) +
                          " agents");
  }
}

} // namespace

ThreadManager makeThreadManager() {
  const char *home = std::getenv("HOME");
  return ThreadManager(std::string(home ? home : "/root") + "/.firmius/threads");
}

std::string requireCurrentThreadId(shared::ToolContext &ctx) {
  const auto &context = ctx.agent.getContext();
  if (!context.history || context.history->threadId.empty()) {
    throw std::runtime_error("No current thread exists");
  }
  return context.history->threadId;
}

uint64_t nowEpochMs() {
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count());
}

WorkAgentRole roleForContext(const shared::ToolContext &ctx) {
  const auto &context = ctx.agent.getContext();
  const std::string &persona = context.config.personaName;
  if (persona == "lead") {
    return WorkAgentRole::Lead;
  }
  if (persona == "executor") {
    return WorkAgentRole::Executor;
  }
  if (persona == "auditor") {
    return WorkAgentRole::Auditor;
  }
  if (persona == "worker") {
    return WorkAgentRole::Worker;
  }
  if (persona == "scout") {
    return WorkAgentRole::Scout;
  }
  return WorkAgentRole::Unknown;
}

std::string roleName(WorkAgentRole role) {
  switch (role) {
  case WorkAgentRole::Lead:
    return "lead";
  case WorkAgentRole::Executor:
    return "executor";
  case WorkAgentRole::Auditor:
    return "auditor";
  case WorkAgentRole::Worker:
    return "worker";
  case WorkAgentRole::Scout:
    return "scout";
  case WorkAgentRole::Unknown:
    return "unknown";
  }
  return "unknown";
}

std::string normalizeWorkRole(const std::string &role,
                              const std::string &fieldName,
                              bool allowEmpty) {
  const std::string trimmed = shared::StringUtil::trim(role);
  if (trimmed.empty()) {
    if (allowEmpty) {
      return "";
    }
    throw roleError(fieldName + " must not be empty");
  }

  const std::string lowered = shared::StringUtil::toLower(trimmed);
  if (lowered == "lead" || lowered == "executor" || lowered == "worker" ||
      lowered == "auditor" || lowered == "scout") {
    return lowered;
  }

  if (lowered == "implementer") {
    throw roleError(fieldName +
                    " uses legacy role 'implementer'; use 'executor'");
  }
  if (lowered == "researcher") {
    throw roleError(fieldName +
                    " uses legacy role 'researcher'; use 'scout'");
  }

  throw roleError(fieldName + " must be one of: lead, executor, worker, "
                            "auditor, scout");
}

std::string normalizePersonaRole(const std::string &persona,
                                 const std::string &fieldName) {
  return normalizeWorkRole(persona, fieldName, false);
}

bool hasScope(const shared::ToolContext &ctx, shared::ToolScope scope) {
  const auto &allowed = ctx.agent.getContext().permissions.allowedScopes;
  return std::find(allowed.begin(), allowed.end(), scope) != allowed.end();
}

void requirePlanReadAccess(const shared::ToolContext &ctx) {
  requireScope(ctx, shared::ToolScope::PlanRead, "plan reads");
  switch (roleForContext(ctx)) {
  case WorkAgentRole::Lead:
  case WorkAgentRole::Executor:
  case WorkAgentRole::Auditor:
  case WorkAgentRole::Scout:
    return;
  default:
    throw permissionError("plan reads are not allowed for " +
                          roleName(roleForContext(ctx)) + " agents");
  }
}

void requirePlanWriteAccess(const shared::ToolContext &ctx) {
  requireScope(ctx, shared::ToolScope::PlanWrite, "plan mutations");
  requireRole(ctx, WorkAgentRole::Lead, "plan mutations");
}

void requireChunkReadAccess(const shared::ToolContext &ctx) {
  requireScope(ctx, shared::ToolScope::ChunkRead, "chunk reads");
  switch (roleForContext(ctx)) {
  case WorkAgentRole::Lead:
  case WorkAgentRole::Executor:
  case WorkAgentRole::Auditor:
  case WorkAgentRole::Scout:
    return;
  default:
    throw permissionError("chunk reads are not allowed for " +
                          roleName(roleForContext(ctx)) + " agents");
  }
}

void requireChunkAddAccess(const shared::ToolContext &ctx) {
  requireScope(ctx, shared::ToolScope::ChunkWrite, "chunk creation");
  requireRole(ctx, WorkAgentRole::Lead, "chunk creation");
}

void requireChunkUpdateAccess(const rapidjson::Value &input,
                              const shared::ToolContext &ctx,
                              const std::string &,
                              const shared::Plan &,
                              const shared::WorkChunk &chunk) {
  const auto role = roleForContext(ctx);
  const auto fields = requestedChunkUpdateFields(input);

  if (fields.empty()) {
    throw permissionError("chunk update requires at least one mutable field");
  }

  static const std::set<std::string> kExecutorFields = {
      "status", "attempt_count", "result_summary"};
  static const std::set<std::string> kAuditorFields = {"review_summary"};
  // V2 rich chunk spec fields are lead-only
  static const std::set<std::string> kV2Fields = {
      "files_to_read", "files_to_touch", "cwd",
      "verification_condition", "handoff_notes"};

  switch (role) {
  case WorkAgentRole::Lead:
    requireScope(ctx, shared::ToolScope::ChunkWrite, "chunk updates");
    return;
  case WorkAgentRole::Executor:
    requireScope(ctx, shared::ToolScope::ChunkWrite, "executor chunk updates");
    if (chunk.assignedAgentId != ctx.agent.getContext().identity.id) {
      throw permissionError("executor may update only its assigned chunk");
    }
    // Executor cannot mutate V2 rich spec fields
    for (const auto &field : fields) {
      if (kV2Fields.count(field) > 0) {
        throw permissionError(
            "executor may not mutate V2 chunk spec fields: " + field);
      }
    }
    if (!std::includes(kExecutorFields.begin(), kExecutorFields.end(),
                       fields.begin(), fields.end())) {
      throw permissionError(
          "executor may update only status, attempt_count, and result_summary");
    }
    return;
  case WorkAgentRole::Auditor:
    requireScope(ctx, shared::ToolScope::ChunkReview, "auditor chunk reviews");
    // Auditor cannot mutate V2 rich spec fields
    for (const auto &field : fields) {
      if (kV2Fields.count(field) > 0) {
        throw permissionError(
            "auditor may not mutate V2 chunk spec fields: " + field);
      }
    }
    if (!std::includes(kAuditorFields.begin(), kAuditorFields.end(),
                       fields.begin(), fields.end())) {
      throw permissionError(
          "auditor may update only review_summary in V1");
    }
    return;
  case WorkAgentRole::Worker:
  case WorkAgentRole::Scout:
  case WorkAgentRole::Unknown:
    throw permissionError("chunk updates are not allowed for " +
                          roleName(role) + " agents");
  }
}

void validateExecutorAssignmentInvariant(ThreadManager &tm,
                                         const std::string &threadId,
                                         const std::string &planId,
                                         const std::string &chunkId,
                                         const std::string &agentId) {
  if (agentId.empty()) {
    return;
  }

  for (const auto &candidatePlan : tm.listPlans(threadId)) {
    for (const auto &candidateChunk : candidatePlan.chunks) {
      if (candidatePlan.id == planId && candidateChunk.id == chunkId) {
        if (!candidateChunk.assignedAgentId.empty() &&
            candidateChunk.assignedAgentId != agentId) {
          throw permissionError("chunk '" + chunkId +
                                "' is already assigned to executor '" +
                                candidateChunk.assignedAgentId + "'");
        }
        continue;
      }
      if (candidateChunk.assignedAgentId != agentId) {
        continue;
      }
      throw permissionError("executor '" + agentId +
                            "' already owns chunk '" + candidateChunk.id + "'");
    }
  }
}

std::string planStatusToString(shared::PlanStatus status) {
  switch (status) {
  case shared::PlanStatus::Draft:
    return "Draft";
  case shared::PlanStatus::Active:
    return "Active";
  case shared::PlanStatus::Paused:
    return "Paused";
  case shared::PlanStatus::Done:
    return "Done";
  case shared::PlanStatus::Abandoned:
    return "Abandoned";
  }
  return "Draft";
}

shared::PlanStatus parsePlanStatus(const std::string &status) {
  if (status == "Draft") {
    return shared::PlanStatus::Draft;
  }
  if (status == "Active") {
    return shared::PlanStatus::Active;
  }
  if (status == "Paused") {
    return shared::PlanStatus::Paused;
  }
  if (status == "Done") {
    return shared::PlanStatus::Done;
  }
  if (status == "Abandoned") {
    return shared::PlanStatus::Abandoned;
  }
  throw std::runtime_error("Unsupported plan status: " + status);
}

std::string chunkStatusToString(shared::WorkChunkStatus status) {
  switch (status) {
  case shared::WorkChunkStatus::Ready:
    return "Ready";
  case shared::WorkChunkStatus::InProgress:
    return "InProgress";
  case shared::WorkChunkStatus::Implemented:
    return "Implemented";
  case shared::WorkChunkStatus::Verifying:
    return "Verifying";
  case shared::WorkChunkStatus::Done:
    return "Done";
  case shared::WorkChunkStatus::Blocked:
    return "Blocked";
  case shared::WorkChunkStatus::Failed:
    return "Failed";
  case shared::WorkChunkStatus::Cancelled:
    return "Cancelled";
  }
  return "Ready";
}

shared::WorkChunkStatus parseChunkStatus(const std::string &status) {
  if (status == "Draft") {
    return shared::WorkChunkStatus::Ready;
  }
  if (status == "Ready") {
    return shared::WorkChunkStatus::Ready;
  }
  if (status == "InProgress") {
    return shared::WorkChunkStatus::InProgress;
  }
  if (status == "Implemented") {
    return shared::WorkChunkStatus::Implemented;
  }
  if (status == "Verifying") {
    return shared::WorkChunkStatus::Verifying;
  }
  if (status == "Done") {
    return shared::WorkChunkStatus::Done;
  }
  if (status == "Blocked") {
    return shared::WorkChunkStatus::Blocked;
  }
  if (status == "Failed") {
    return shared::WorkChunkStatus::Failed;
  }
  if (status == "Cancelled") {
    return shared::WorkChunkStatus::Cancelled;
  }
  throw std::runtime_error("Unsupported chunk status: " + status);
}

std::vector<std::string> parseStringArray(const rapidjson::Value &input,
                                          const char *key) {
  std::vector<std::string> values;
  if (!input.HasMember(key)) {
    return values;
  }

  for (const auto &item : input[key].GetArray()) {
    values.emplace_back(item.GetString());
  }
  return values;
}

shared::Plan loadPlan(ThreadManager &tm, const std::string &threadId,
                      const std::string &planId) {
  return tm.getPlan(threadId, planId);
}

shared::WorkChunk &requireChunk(shared::Plan &plan, const std::string &chunkId) {
  auto it = std::find_if(plan.chunks.begin(), plan.chunks.end(),
                         [&](const shared::WorkChunk &chunk) {
                           return chunk.id == chunkId;
                         });
  if (it == plan.chunks.end()) {
    throw std::runtime_error("Chunk not found: " + chunkId);
  }
  return *it;
}

const shared::WorkChunk &requireChunk(const shared::Plan &plan,
                                      const std::string &chunkId) {
  auto it = std::find_if(plan.chunks.begin(), plan.chunks.end(),
                         [&](const shared::WorkChunk &chunk) {
                           return chunk.id == chunkId;
                         });
  if (it == plan.chunks.end()) {
    throw std::runtime_error("Chunk not found: " + chunkId);
  }
  return *it;
}

bool chunkDependenciesDone(const shared::Plan &plan,
                           const shared::WorkChunk &chunk) {
  for (const auto &dependencyId : chunk.dependsOn) {
    auto it = std::find_if(plan.chunks.begin(), plan.chunks.end(),
                           [&](const shared::WorkChunk &candidate) {
                             return candidate.id == dependencyId;
                           });
    if (it == plan.chunks.end() || it->status != shared::WorkChunkStatus::Done) {
      return false;
    }
  }

  return true;
}

bool blockChunkIfDependenciesIncomplete(const shared::Plan &plan,
                                        shared::WorkChunk &chunk) {
  if (chunk.status != shared::WorkChunkStatus::Ready ||
      chunkDependenciesDone(plan, chunk)) {
    return false;
  }

  chunk.status = shared::WorkChunkStatus::Blocked;
  return true;
}

bool chunkReadyForExecution(const shared::Plan &plan,
                            const shared::WorkChunk &chunk) {
  return chunk.status == shared::WorkChunkStatus::Ready &&
         chunkDependenciesDone(plan, chunk);
}

void requireChunkReadyForExecution(const shared::Plan &plan,
                                   const shared::WorkChunk &chunk,
                                   const std::string &action) {
  if (chunk.status != shared::WorkChunkStatus::Ready) {
    throw std::runtime_error("Chunk '" + chunk.id + "' is not ready for " +
                             action + ": status is " +
                             chunkStatusToString(chunk.status) +
                             "; chunk must be Ready and all dependencies must be Done");
  }

  for (const auto &dependencyId : chunk.dependsOn) {
    auto it = std::find_if(plan.chunks.begin(), plan.chunks.end(),
                           [&](const shared::WorkChunk &candidate) {
                             return candidate.id == dependencyId;
                           });
    if (it == plan.chunks.end()) {
      throw std::runtime_error("Chunk '" + chunk.id + "' is not ready for " +
                               action + ": dependency '" + dependencyId +
                               "' was not found");
    }
    if (it->status != shared::WorkChunkStatus::Done) {
      throw std::runtime_error("Chunk '" + chunk.id + "' is not ready for " +
                               action + ": dependency '" + dependencyId +
                               "' is " + chunkStatusToString(it->status) +
                               "; dependencies must be Done");
    }
  }
}

rapidjson::Value makePlanSummary(const shared::Plan &plan, bool isActive,
                                 rapidjson::Document::AllocatorType &alloc) {
  rapidjson::Value summary(rapidjson::kObjectType);
  summary.AddMember("plan_id", rapidjson::Value(plan.id.c_str(), alloc), alloc);
  summary.AddMember("title", rapidjson::Value(plan.title.c_str(), alloc), alloc);
  std::string status = planStatusToString(plan.status);
  summary.AddMember("status", rapidjson::Value(status.c_str(), alloc), alloc);
  summary.AddMember("is_active", isActive, alloc);
  summary.AddMember("updated_at", plan.updatedAt, alloc);
  return summary;
}

rapidjson::Value makeChunkSummary(const shared::WorkChunk &chunk,
                                  rapidjson::Document::AllocatorType &alloc) {
  rapidjson::Value summary(rapidjson::kObjectType);
  summary.AddMember("chunk_id", rapidjson::Value(chunk.id.c_str(), alloc), alloc);
  summary.AddMember("title", rapidjson::Value(chunk.title.c_str(), alloc), alloc);
  summary.AddMember("planning_gate", chunk.planningGate, alloc);
  std::string status = chunkStatusToString(chunk.status);
  summary.AddMember("status", rapidjson::Value(status.c_str(), alloc), alloc);

  rapidjson::Value dependsOn(rapidjson::kArrayType);
  for (const auto &dependency : chunk.dependsOn) {
    dependsOn.PushBack(rapidjson::Value(dependency.c_str(), alloc), alloc);
  }
  summary.AddMember("depends_on", dependsOn, alloc);
  summary.AddMember("assigned_agent_id",
                    rapidjson::Value(chunk.assignedAgentId.c_str(), alloc),
                    alloc);
  return summary;
}

void emitWorkEvent(const shared::AppEvent &event) {
  Harness::instance().publishEvent(event);
}

} // namespace firmius::core::worktools

#include "tools/WorkToolCommon.hpp"
#include "agents/PurposeLoader.hpp"
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

bool shouldTreatAsRequestedField(const rapidjson::Value &input,
                                 const char *field) {
  if (!input.HasMember(field)) {
    return false;
  }

  const auto &value = input[field];
  if (value.IsNull()) {
    return false;
  }
  if (value.IsString()) {
    return !shared::StringUtil::trim(std::string_view(value.GetString()))
                .empty();
  }
  if (value.IsArray()) {
    return !value.Empty();
  }
  return true;
}

std::set<std::string> requestedChunkUpdateFields(const rapidjson::Value &input) {
  static const std::vector<std::string> kMutableFields = {
      "title",         "goal",          "context",      "constraints",
      "completion",    "planning_gate", "status",       "depends_on",
      "attempt_count", "result_summary", "review_summary", "assigned_agent_id",
      // V2 rich chunk spec fields
      "files_to_read", "files_to_touch", "cwd", "tasks",
      "verification_condition", "handoff_notes"};

  std::set<std::string> fields;
  for (const auto &field : kMutableFields) {
    if (field == "assigned_agent_id" && input.HasMember(field.c_str())) {
      fields.insert(field);
      continue;
    }
    if (shouldTreatAsRequestedField(input, field.c_str())) {
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

std::string lockStatusOrDefault(const FleetLock &lock) {
  if (!lock.status.empty()) {
    return lock.status;
  }
  return "open";
}

} // namespace

ThreadManager makeThreadManager() {
  return ThreadManager(ThreadManager::defaultBasePath());
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
  if (context.config.personaName.empty()) {
    return WorkAgentRole::Unknown;
  }
  switch (PurposeLoader::resolveWorkRole(context.config.personaName)) {
  case PurposeWorkRole::Lead:
    return WorkAgentRole::Lead;
  case PurposeWorkRole::Executor:
    return WorkAgentRole::Executor;
  case PurposeWorkRole::Worker:
    return WorkAgentRole::Worker;
  case PurposeWorkRole::Auditor:
    return WorkAgentRole::Auditor;
  case PurposeWorkRole::Scout:
    return WorkAgentRole::Scout;
  case PurposeWorkRole::Unknown:
    return WorkAgentRole::Unknown;
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
      "files_to_read", "files_to_touch", "cwd", "tasks",
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

std::vector<shared::WorkTask> parseTaskArray(const rapidjson::Value &input,
                                             const char *key) {
  std::vector<shared::WorkTask> tasks;
  if (!input.HasMember(key) || !input[key].IsArray()) {
    return tasks;
  }

  for (const auto &item : input[key].GetArray()) {
    if (!item.IsObject()) {
      throw std::runtime_error(std::string(key) +
                               " must contain only object entries");
    }

    if (!item.HasMember("id") || !item["id"].IsString()) {
      throw std::runtime_error("task.id is required and must be a string");
    }
    if (!item.HasMember("title") || !item["title"].IsString()) {
      throw std::runtime_error("task.title is required and must be a string");
    }
    if (!item.HasMember("goal") || !item["goal"].IsString()) {
      throw std::runtime_error("task.goal is required and must be a string");
    }

    shared::WorkTask task;
    task.id = item["id"].GetString();
    task.title = item["title"].GetString();
    task.goal = item["goal"].GetString();
    task.status =
        item.HasMember("status") && item["status"].IsString()
            ? parseChunkStatus(item["status"].GetString())
            : shared::WorkChunkStatus::Ready;
    task.notes = item.HasMember("notes") && item["notes"].IsString()
                     ? item["notes"].GetString()
                     : "";
    task.verificationCondition =
        item.HasMember("verification_condition") &&
                item["verification_condition"].IsString()
            ? item["verification_condition"].GetString()
            : "";
    task.assignedWorkerId =
        item.HasMember("assigned_worker_id") &&
                item["assigned_worker_id"].IsString()
            ? item["assigned_worker_id"].GetString()
            : "";
    task.createdAt = nowEpochMs();
    task.updatedAt = task.createdAt;
    tasks.push_back(std::move(task));
  }

  return tasks;
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

bool unblockChunkIfDependenciesMet(const shared::Plan &plan,
                                   shared::WorkChunk &chunk) {
  if (chunk.status == shared::WorkChunkStatus::Blocked &&
      chunkDependenciesDone(plan, chunk)) {
    chunk.status = shared::WorkChunkStatus::Ready;
    return true;
  }
  return false;
}

bool chunkReadyForExecution(const shared::Plan &plan,
                            const shared::WorkChunk &chunk) {
  return chunk.status == shared::WorkChunkStatus::Ready &&
         chunkDependenciesDone(plan, chunk);
}

bool unblockDependentChunks(shared::Plan &plan,
                            const std::string &chunkId) {
  bool changed = false;
  for (auto &chunk : plan.chunks) {
    if (chunk.status == shared::WorkChunkStatus::Blocked) {
      bool dependsOnDone = false;
      for (const auto &depId : chunk.dependsOn) {
        if (depId == chunkId) {
          dependsOnDone = true;
          break;
        }
      }
      if (dependsOnDone && chunkDependenciesDone(plan, chunk)) {
        chunk.status = shared::WorkChunkStatus::Ready;
        chunk.updatedAt = nowEpochMs();
        changed = true;
      }
    }
  }
  return changed;
}

bool reconcileChunkDependencies(shared::Plan &plan) {
  bool changed = false;
  for (auto &chunk : plan.chunks) {
    if (chunk.status == shared::WorkChunkStatus::Blocked) {
      if (chunkDependenciesDone(plan, chunk)) {
        chunk.status = shared::WorkChunkStatus::Ready;
        chunk.updatedAt = nowEpochMs();
        changed = true;
      }
    }
  }
  return changed;
}

void requireChunkReadyForExecution(const shared::Plan &plan,
                                   const shared::WorkChunk &chunk,
                                   const std::string &action) {
  if (chunk.status != shared::WorkChunkStatus::Ready) {
    std::string dependencyDetail;
    if (chunk.status == shared::WorkChunkStatus::Blocked) {
      for (const auto &dependencyId : chunk.dependsOn) {
        auto it = std::find_if(plan.chunks.begin(), plan.chunks.end(),
                               [&](const shared::WorkChunk &candidate) {
                                 return candidate.id == dependencyId;
                               });
        if (it == plan.chunks.end()) {
          dependencyDetail =
              "; unresolved dependency '" + dependencyId + "' was not found";
          break;
        }
        if (it->status != shared::WorkChunkStatus::Done) {
          dependencyDetail = "; unresolved dependency '" + dependencyId +
                             "' is " + chunkStatusToString(it->status);
          break;
        }
      }
    }
    throw std::runtime_error("Chunk '" + chunk.id + "' is not ready for " +
                             action + ": status is " +
                             chunkStatusToString(chunk.status) +
                             "; chunk must be Ready and all dependencies must be Done" +
                             dependencyDetail);
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
  summary.AddMember("task_count", static_cast<uint64_t>(chunk.tasks.size()),
                    alloc);
  return summary;
}

void emitWorkEvent(const shared::AppEvent &event) {
  Harness::instance().publishEvent(event);
}

std::vector<FileConflictInfo> checkFileConflicts(
    const std::string &threadId,
    const std::vector<std::string> &filePaths,
    const std::string &requestingAgentId) {
  std::vector<FileConflictInfo> conflicts;
  ThreadManager tm(ThreadManager::defaultBasePath());
  FleetState state = tm.getFleetState(threadId);
  
  for (const auto &lock : state.locks) {
    const std::string status = lockStatusOrDefault(lock);
    if (status != "open") {
      continue;  // Skip closed locks
    }
    
    // Skip locks owned by the requesting agent
    if (lock.ownerAgentId == requestingAgentId) {
      continue;
    }
    
    // Check if lock covers any of the requested files
    if (lock.paths.empty()) {
      // Lock with no paths covers everything
      for (const auto &filePath : filePaths) {
        conflicts.push_back({filePath, lock.lockId, lock.ownerAgentId, lock.reason});
      }
    } else {
      for (const auto &lockPath : lock.paths) {
        for (const auto &filePath : filePaths) {
          if (lockPath == filePath) {
            conflicts.push_back({filePath, lock.lockId, lock.ownerAgentId, lock.reason});
            break;
          }
        }
      }
    }
  }
  
  return conflicts;
}

std::string acquireFileLock(
    const std::string &threadId,
    const std::vector<std::string> &filePaths,
    const std::string &reason,
    const std::string &ownerAgentId,
    int /*timeoutMs*/) {
  ThreadManager tm(ThreadManager::defaultBasePath());
  
  FleetLock lock;
  lock.lockId = shared::StringUtil::generateUuid();
  lock.threadId = threadId;
  lock.ownerAgentId = ownerAgentId;
  lock.rootAgentId = ownerAgentId;  // Simplified: assume owner is root
  lock.reason = reason;
  lock.paths = filePaths;
  lock.status = "open";
  lock.createdAt = nowEpochMs();
  lock.updatedAt = lock.createdAt;
  
  tm.mutateFleetState(threadId, [&](FleetState &state) {
    state.locks.push_back(lock);
  });
  
  return lock.lockId;
}

bool releaseFileLock(const std::string &threadId, const std::string &lockId) {
  ThreadManager tm(ThreadManager::defaultBasePath());
  bool found = false;
  
  tm.mutateFleetState(threadId, [&](FleetState &state) {
    for (auto &lock : state.locks) {
      if (lock.lockId == lockId) {
        lock.status = "released";
        lock.updatedAt = nowEpochMs();
        found = true;
        break;
      }
    }
  });
  
  return found;
}

std::string buildExecutorLockDoctrine() {
  return R"(
## File Lock Protocol for Executors

You coordinate file access when delegating to workers using `fleet_lock`. Follow this protocol:

### 1. Plan Before Delegating
- Identify all files your workers will need to read or modify
- Check for existing locks: `fleet_lock` with `{"mode": "check", "paths": [...]}`
- If conflicts exist, wait or reassign workers to non-conflicting files

### 2. Acquire Locks Before Spawning Workers
- Use `fleet_lock` with `{"mode": "acquire", "reason": "...", "paths": [...]}`
- Example: `{"mode": "acquire", "reason": "Refactoring auth module", "paths": ["src/auth.cpp", "src/auth.hpp"]}`
- Keep locks narrow: only files being actively modified
- Store the returned `lock_id` for release later

### 3. Assign Workers to Non-Overlapping Files
- Each worker should touch a distinct set of files
- Pass explicit file lists to workers in their task description
- Example: "Worker A: modify src/auth/login.cpp only. Worker B: modify src/auth/session.cpp only"

### 4. Handle Conflicts via Lock Requests
- If workers need files locked by others, use `fleet_lock` with `mode: "request"`
- Example: `{"mode": "request", "target_agent_id": "worker-b", "paths": ["src/auth.cpp"], "reason": "Need auth changes"}`
- This blocks until the target worker completes and releases their lock

### 5. Release Locks Promptly
- Call `fleet_lock` with `{"mode": "release", "lock_id": "..."}` when all workers complete
- Do not hold locks longer than necessary

### Lock Lifecycle Pattern
```
1. fleet_lock({mode: "check", paths: [...]}) → detect conflicts
2. fleet_lock({mode: "acquire", reason: "...", paths: [...]}) → get lock_id
3. summon_subagent(worker, task_with_files) → delegate work
4. Wait for worker completion
5. fleet_lock({mode: "release", lock_id: "..."}) → free resources
```

### Tool Reference
- `{"mode": "acquire", "reason": "...", "paths": [...], "timeout_ms": N}` - Acquire lock, wait up to N ms if contested
- `{"mode": "release", "lock_id": "..."}` - Release a lock you own
- `{"mode": "request", "target_agent_id": "...", "paths": [...], "reason": "..."}` - Request another agent to lock files
- `{"mode": "wait", "lock_id": "...", "timeout_ms": N}` - Wait for specific lock to release
- `{"mode": "check", "paths": [...]}` - Check if paths have active locks
)";
}

std::string buildWorkerLockDoctrine() {
  return R"(
## File Lock Protocol for Workers

You are a worker executing a bounded subtask. Coordinate file access WITH OTHER WORKERS directly using `fleet_lock` - do not bother your parent executor with lock conflicts.

### 1. Check Before Editing
- Before ANY file edit, run `fleet_lock` with `{"mode": "check", "paths": [...]}`
- Example: `{"mode": "check", "paths": ["src/auth/login.cpp"]}`

### 2. If Files Are Locked - WAIT, Don't Report
- If check shows another agent holds a lock:
  - Use `fleet_lock` with `mode: "acquire"` and `timeout_ms` to wait
  - Example: `{"mode": "acquire", "reason": "Editing auth", "paths": ["src/auth.cpp"], "timeout_ms": 60000}`
  - The tool blocks until the lock is released or timeout expires
  - Only report if timeout expires

### 3. Acquire Locks Before Editing
- Always call `fleet_lock` with `mode: "acquire"` before making file changes
- Include your specific files in `paths`
- Use `timeout_ms` when you expect brief contention (30000-120000ms typical)
- The lock ensures no other worker can edit those files simultaneously

### 4. Keep Locks Narrow and Brief
- Lock only the exact files you are modifying
- Do not lock files you only read
- Release locks immediately after completing edits: `{"mode": "release", "lock_id": "..."}`

### 5. Self-Service Conflict Resolution
- Workers coordinate directly through the lock system
- No need to notify parent executor about lock waits or acquisitions
- Only mention locks in your final summary if relevant

### Worker Lock Flow (Self-Service)
```
Start task
    ↓
Check: fleet_lock({mode: "check", paths: [...]})
    ↓
has_conflicts?
    │
    ├─YES─→ Acquire with timeout: fleet_lock({mode: "acquire", reason: "...", paths: [...], timeout_ms: N})
    │         ↓
    │       Timeout? ──YES──→ Report "timed out waiting for lock on [files]"
    │         │
    │         NO (lock acquired)
    │         ↓
    └─NO──→ Acquire: fleet_lock({mode: "acquire", reason: "...", paths: [...]})
              ↓
Do your edits
    ↓
Release: fleet_lock({mode: "release", lock_id: "..."})
    ↓
Report completion (no lock details needed unless asked)
```

### Example Worker Flow
```
1. "Checking for file conflicts"
   → fleet_lock({mode: "check", paths: ["src/auth/login.cpp"]})
   → Result: has_conflicts: true, owner: "worker-xyz"

2. "Waiting for lock on src/auth/login.cpp"
   → fleet_lock({mode: "acquire", reason: "Auth refactor", paths: ["src/auth/login.cpp"], timeout_ms: 60000})
   → [blocks until worker-xyz finishes]
   → Result: lock_id: "abc123"

3. [Perform file edits]

4. "Releasing lock"
   → fleet_lock({mode: "release", lock_id: "abc123"})

5. "Task complete: modified src/auth/login.cpp"
   (no need to mention lock coordination - it's handled automatically)
```

### Key Principle
**Workers coordinate file access directly through locks.** The parent executor delegates work and trusts workers to handle file conflicts autonomously. Only report if a lock timeout expires after a reasonable wait.

### Tool Reference
- `{"mode": "acquire", "reason": "...", "paths": [...], "timeout_ms": N}` - Acquire lock, wait up to N ms if contested
- `{"mode": "release", "lock_id": "..."}` - Release a lock you own
- `{"mode": "check", "paths": [...]}` - Check if paths have active locks
- `{"mode": "wait", "lock_id": "...", "timeout_ms": N}` - Wait for specific lock to release
)";
}

} // namespace firmius::core::worktools

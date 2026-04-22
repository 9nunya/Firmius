#include "tools/WorkTool.hpp"

#include "Serialization.hpp"
#include "agents/PurposeLoader.hpp"
#include "harness/Harness.hpp"
#include "persistence/ThreadManager.hpp"
#include "utils/StringUtil.hpp"

#include <algorithm>
#include <chrono>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace firmius::core {

namespace {

// ===================== Core helpers (embedded; no legacy deps) =====================

std::runtime_error permissionError(const std::string &message) {
  return std::runtime_error("Work authority denied: " + message);
}

[[maybe_unused]] std::runtime_error roleError(const std::string &message) {
  return std::runtime_error("Invalid work role: " + message);
}

std::string requireString(const rapidjson::Value &input, const char *key) {
  if (!input.HasMember(key) || !input[key].IsString()) {
    throw std::runtime_error(std::string("Missing required field: ") + key);
  }
  return input[key].GetString();
}

std::string actionOf(const rapidjson::Value &input) {
  return requireString(input, "action");
}

// ---- Roles/scopes ----

enum class WorkAgentRole { Lead, Executor, Auditor, Worker, Scout, Unknown };

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
    return "aster";
  case WorkAgentRole::Executor:
    return "forge";
  case WorkAgentRole::Auditor:
    return "witness";
  case WorkAgentRole::Worker:
    return "ember";
  case WorkAgentRole::Scout:
    return "glimmer";
  case WorkAgentRole::Unknown:
    return "unknown";
  }
  return "unknown";
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

bool hasScope(const shared::ToolContext &ctx, shared::ToolScope scope) {
  const auto &allowed = ctx.agent.getContext().permissions.allowedScopes;
  return std::find(allowed.begin(), allowed.end(), scope) != allowed.end();
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

// ---- Thread / time ----

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

// ---- Status conversion ----

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

// ---- Parsing helpers ----

bool shouldApplyOptionalField(const rapidjson::Value &input, const char *field) {
  if (!input.HasMember(field)) {
    return false;
  }

  const auto &value = input[field];
  if (value.IsNull()) {
    return false;
  }
  if (value.IsString()) {
    return !shared::StringUtil::trim(std::string_view(value.GetString())).empty();
  }
  if (value.IsArray()) {
    return !value.Empty();
  }
  return true;
}

bool shouldTreatAsRequestedField(const rapidjson::Value &input, const char *field) {
  // Mirrors WorkToolCommon behavior: string fields must be non-empty when auto-filled.
  return shouldApplyOptionalField(input, field);
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

// ---- Plan/chunk selection ----

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

const shared::WorkChunk *findChunkById(const shared::Plan &plan,
                                       std::string_view chunkId) {
  auto it = std::find_if(plan.chunks.begin(), plan.chunks.end(),
                         [&](const shared::WorkChunk &chunk) {
                           return chunk.id == chunkId;
                         });
  if (it == plan.chunks.end()) {
    return nullptr;
  }
  return &*it;
}

const shared::WorkChunk *findChunkByUniqueTitle(const shared::Plan &plan,
                                                std::string_view title) {
  const shared::WorkChunk *match = nullptr;
  for (const auto &chunk : plan.chunks) {
    if (chunk.title != title) {
      continue;
    }
    if (match != nullptr) {
      return nullptr;
    }
    match = &chunk;
  }
  return match;
}

const shared::WorkChunk *findDependencyChunk(const shared::Plan &plan,
                                             const std::string &dependencyRef) {
  if (const auto *byId = findChunkById(plan, dependencyRef)) {
    return byId;
  }
  return findChunkByUniqueTitle(plan, dependencyRef);
}

// ---- Dependency invariants / ready-for-execution ----

bool chunkDependenciesDone(const shared::Plan &plan,
                           const shared::WorkChunk &chunk) {
  for (const auto &dependencyId : chunk.dependsOn) {
    const auto *dependency = findDependencyChunk(plan, dependencyId);
    if (dependency == nullptr ||
        dependency->status != shared::WorkChunkStatus::Done) {
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

bool unblockDependentChunks(shared::Plan &plan, const std::string &chunkId) {
  bool changed = false;
  for (auto &chunk : plan.chunks) {
    if (chunk.status == shared::WorkChunkStatus::Blocked) {
      bool dependsOnDone = false;
      for (const auto &dependencyRef : chunk.dependsOn) {
        const auto *dependency = findDependencyChunk(plan, dependencyRef);
        if (dependency != nullptr && dependency->id == chunkId) {
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

bool chunkReadyForExecution(const shared::Plan &plan,
                            const shared::WorkChunk &chunk) {
  return chunk.status == shared::WorkChunkStatus::Ready &&
         chunkDependenciesDone(plan, chunk);
}

// ---- Chunk update permissions ----

bool hasMeaningfulMutationOtherThanAssignment(const rapidjson::Value &input) {
  static const std::vector<const char *> kOtherFields = {
      "title",      "goal",     "context",     "constraints",
      "completion", "planning_gate", "status", "depends_on",
      "attempt_count", "result_summary", "review_summary",
      "files_to_read", "files_to_touch", "cwd", "tasks",
      "verification_condition", "handoff_notes"};

  for (const char *field : kOtherFields) {
    if (shouldApplyOptionalField(input, field)) {
      return true;
    }
  }
  return false;
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
      const auto &value = input[field.c_str()];
      if (!value.IsString()) {
        fields.insert(field);
        continue;
      }
      const bool empty =
          shared::StringUtil::trim(std::string_view(value.GetString())).empty();
      if (!empty || !hasMeaningfulMutationOtherThanAssignment(input)) {
        fields.insert(field);
      }
      continue;
    }
    if (shouldApplyOptionalField(input, field.c_str())) {
      fields.insert(field);
    }
  }
  return fields;
}

void requireChunkUpdateAccess(const rapidjson::Value &input,
                              const shared::ToolContext &ctx,
                              const std::string &threadId,
                              const shared::Plan &plan,
                              const shared::WorkChunk &chunk) {
  (void)threadId;
  (void)plan;
  const auto role = roleForContext(ctx);
  auto fields = requestedChunkUpdateFields(input);

  auto eraseIfUnchanged = [&](const char *field, const auto &currentValue,
                              const auto &incomingValue) {
    if (incomingValue == currentValue) {
      fields.erase(field);
    }
  };

  if (fields.count("title") && shouldApplyOptionalField(input, "title")) {
    eraseIfUnchanged("title", chunk.title,
                     std::string(input["title"].GetString()));
  }
  if (fields.count("goal") && shouldApplyOptionalField(input, "goal")) {
    eraseIfUnchanged("goal", chunk.goal, std::string(input["goal"].GetString()));
  }
  if (fields.count("context") && shouldApplyOptionalField(input, "context")) {
    eraseIfUnchanged("context", chunk.context,
                     std::string(input["context"].GetString()));
  }
  if (fields.count("constraints") &&
      shouldApplyOptionalField(input, "constraints")) {
    eraseIfUnchanged("constraints", chunk.constraints,
                     std::string(input["constraints"].GetString()));
  }
  if (fields.count("completion") &&
      shouldApplyOptionalField(input, "completion")) {
    eraseIfUnchanged("completion", chunk.completion,
                     std::string(input["completion"].GetString()));
  }
  if (fields.count("planning_gate") && input.HasMember("planning_gate")) {
    eraseIfUnchanged("planning_gate", chunk.planningGate,
                     input["planning_gate"].GetBool());
  }
  if (fields.count("status") && shouldApplyOptionalField(input, "status")) {
    eraseIfUnchanged("status", chunk.status,
                     parseChunkStatus(input["status"].GetString()));
  }
  if (fields.count("depends_on") && shouldApplyOptionalField(input, "depends_on")) {
    eraseIfUnchanged("depends_on", chunk.dependsOn,
                     parseStringArray(input, "depends_on"));
  }
  if (fields.count("attempt_count") && input.HasMember("attempt_count")) {
    eraseIfUnchanged("attempt_count", chunk.attemptCount,
                     input["attempt_count"].GetInt());
  }
  if (fields.count("result_summary") &&
      shouldApplyOptionalField(input, "result_summary")) {
    eraseIfUnchanged("result_summary", chunk.resultSummary,
                     std::string(input["result_summary"].GetString()));
  }
  if (fields.count("review_summary") &&
      shouldApplyOptionalField(input, "review_summary")) {
    eraseIfUnchanged("review_summary", chunk.reviewSummary,
                     std::string(input["review_summary"].GetString()));
  }
  if (fields.count("assigned_agent_id") && input.HasMember("assigned_agent_id") &&
      input["assigned_agent_id"].IsString()) {
    eraseIfUnchanged("assigned_agent_id", chunk.assignedAgentId,
                     std::string(input["assigned_agent_id"].GetString()));
  }

  if (fields.empty()) {
    throw permissionError("chunk update requires at least one mutable field");
  }

  static const std::set<std::string> kExecutorFields = {
      "status", "attempt_count", "result_summary"};
  static const std::set<std::string> kAuditorFields = {"review_summary"};
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
    for (const auto &field : fields) {
      if (kV2Fields.count(field) > 0) {
        throw permissionError(
            "auditor may not mutate V2 chunk spec fields: " + field);
      }
    }
    if (!std::includes(kAuditorFields.begin(), kAuditorFields.end(),
                       fields.begin(), fields.end())) {
      throw permissionError("auditor may update only review_summary in V1");
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

// ---- Events ----

void emitWorkEvent(const shared::AppEvent &event) {
  Harness::instance().publishEvent(event);
}

// ---- JSON summaries ----

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

[[maybe_unused]] void requireChunkReadyForExecution(const shared::Plan &plan,
                                   const shared::WorkChunk &chunk,
                                   const std::string &action) {
  if (chunk.status != shared::WorkChunkStatus::Ready) {
    std::string dependencyDetail;
    if (chunk.status == shared::WorkChunkStatus::Blocked) {
      for (const auto &dependencyId : chunk.dependsOn) {
        const auto *dependency = findDependencyChunk(plan, dependencyId);
        if (dependency == nullptr) {
          dependencyDetail =
              "; unresolved dependency '" + dependencyId + "' was not found";
          break;
        }
        if (dependency->status != shared::WorkChunkStatus::Done) {
          dependencyDetail = "; unresolved dependency '" + dependencyId +
                             "' is " + chunkStatusToString(dependency->status);
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
    const auto *dependency = findDependencyChunk(plan, dependencyId);
    if (dependency == nullptr) {
      throw std::runtime_error("Chunk '" + chunk.id + "' is not ready for " +
                               action + ": dependency '" + dependencyId +
                               "' was not found");
    }
    if (dependency->status != shared::WorkChunkStatus::Done) {
      throw std::runtime_error("Chunk '" + chunk.id + "' is not ready for " +
                               action + ": dependency '" + dependencyId +
                               "' is " +
                               chunkStatusToString(dependency->status) +
                               "; dependencies must be Done");
    }
  }
}

[[maybe_unused]] std::string buildExecutorLockDoctrine() {
  return R"(
## Fleet Coordination Doctrine for Executors

The goal is not "put a generic lock on every file."
The goal is to prevent workers from colliding on unstable shared surfaces during implementation and verification.

### Mental Model

Think in terms of **edit ownership until stable**.
If worker A is still modifying or stabilizing a shared surface, worker B should not race in and "help fix" that same surface during verification.

### Your Responsibilities

1. Prefer task and file separation when delegating.
2. Tell workers their likely edit surfaces up front when you know them.
3. If late-wave collisions happen anyway, have workers coordinate through `fleet_lock` rather than fighting each other through repeated rereads.

### When Workers Should Use `request`

Workers should use `fleet_lock` with `mode: "request"` when:
- another worker is still actively editing or stabilizing a surface they now depend on
- peer edits are causing verification churn
- a worker is about to "fix" a surface another worker is still changing

The request means:
"Please hold ownership of this unstable surface until your current edit/verification wave is done, then release it so I can continue."

### What You Should Expect

When two workers converge on the same unstable surface:
- do NOT encourage both to edit it
- do NOT have one worker blindly patch over the other
- prefer one worker finishing and releasing ownership, then the other rereading and continuing

### Bad Fleet Pattern

Worker 1 finishes implementation, tries to build, sees a failure caused by Worker 2's in-flight edits, and immediately edits the same surface.
This creates churn, reread loops, and false verification failures.

### Good Fleet Pattern

Worker 1 notices Worker 2 is still changing the shared surface.
Worker 1 requests Worker 2 to hold ownership until stable.
Worker 1 waits.
Worker 2 finishes, releases ownership, and Worker 1 rereads and resumes verification.

### Tool Reference
- `{\"mode\":\"check\",\"paths\":[...]}` - inspect active conflicts
- `{\"mode\":\"acquire\",\"reason\":\"...\",\"paths\":[...],\"timeout_ms\":N}` - claim narrow ownership of a surface you are actively editing
- `{\"mode\":\"release\",\"lock_id\":\"...\"}` - release ownership when stable
- `{\"mode\":\"request\",\"target_agent_id\":\"...\",\"paths\":[...],\"reason\":\"...\"}` - ask another worker to hold ownership until their current wave is done
- `{\"mode\":\"wait\",\"lock_id\":\"...\",\"timeout_ms\":N}` - wait on a known lock when appropriate
)";
}

[[maybe_unused]] std::string buildWorkerLockDoctrine() {
  return R"(
## Fleet Coordination Doctrine for Workers

You coordinate directly with peer workers.
The goal is to avoid racing on unstable shared surfaces during implementation and verification.

### Correct Mental Model

Do NOT think:
"Locks mean I should put a generic mutex on every file."

Think:
"If another worker still owns an unstable surface I now depend on, I should let them finish that wave, then reread and continue."

### When to Use `acquire`

Use `acquire` when you are actively editing a narrow surface and want to signal ownership while you are changing it.

Good use:
- you are about to perform a real edit on known files
- you expect brief peer overlap on the same surface

Bad use:
- locking every file you read
- locking broad areas "just in case"

### When to Use `request`

Use `request` when another worker is already the better owner of a contested surface and you need them to finish before you continue.

Examples:
- your build is failing because another worker is still editing the same API surface
- peer edit notices show the exact file you were about to verify against
- you are tempted to patch over a peer's in-flight work

The request means:
"Please hold ownership of this surface until your current edit/verification wave is done, then release it so I can continue."

### Preferred Worker Behavior

1. If you are actively editing a narrow surface, acquire ownership narrowly.
2. If a peer is already stabilizing the shared surface, request ownership hold from them instead of colliding.
3. Wait.
4. Reread after release.
5. Continue with fresh context.

### Good Example

Worker 1 finished implementation and tries to verify.
Worker 2 is still changing a shared file.
Worker 1 does NOT immediately patch the file.
Worker 1 requests Worker 2 to hold ownership until done.
Worker 1 waits, rereads after release, and then continues verification.

### Bad Example

Worker 1 sees a failing build caused by Worker 2's in-flight edits and immediately edits the same file.
Worker 2 rereads, changes again, and both workers churn.

### Reporting Rule

Do not spam your parent executor about routine waits.
Report fleet coordination only when:
- a wait times out
- a request is denied and it blocks progress
- the conflict reveals a real execution issue

### Tool Reference
- `{\"mode\":\"check\",\"paths\":[...]}` - inspect conflicts
- `{\"mode\":\"acquire\",\"reason\":\"...\",\"paths\":[...],\"timeout_ms\":N}` - claim narrow active edit ownership
- `{\"mode\":\"release\",\"lock_id\":\"...\"}` - release ownership once stable
- `{\"mode\":\"request\",\"target_agent_id\":\"...\",\"paths\":[...],\"reason\":\"...\"}` - ask a peer to hold ownership until stable
- `{\"mode\":\"wait\",\"lock_id\":\"...\",\"timeout_ms\":N}` - wait on known ownership state
)";
}

} // namespace

// ==================== WorkTool public API ====================

shared::ToolMetadata WorkTool::getMetadata() const {
  return {"Work",
          "Plan and chunk operations. Use action CreatePlan, ListPlans, GetPlan, UpdatePlan, ActivatePlan, AddChunk, ListChunks, GetChunk, UpdateChunk, or ReadyChunk.",
          shared::ToolScope::PlanRead};
}

std::shared_ptr<shared::JSONSchema> WorkTool::getSchema() const {
  return shared::zObject({
      {"action",
       shared::zEnum({"CreatePlan", "ListPlans", "GetPlan", "UpdatePlan",
                      "ActivatePlan", "AddChunk", "ListChunks", "GetChunk",
                      "UpdateChunk", "ReadyChunk"})
           ->describe("Work operation to execute")},
      {"plan_id", shared::zString()->setOptional()},
      {"chunk_id", shared::zString()->setOptional()},
      {"title", shared::zString()->setOptional()},
      {"objective", shared::zString()->setOptional()},
      {"context", shared::zString()->setOptional()},
      {"strategy", shared::zString()->setOptional()},
      {"notes", shared::zString()->setOptional()},
      {"status", shared::zString()->setOptional()},
      {"set_active", shared::zBoolean()->setOptional()},
      {"goal", shared::zString()->setOptional()},
      {"constraints", shared::zString()->setOptional()},
      {"completion", shared::zString()->setOptional()},
      {"planning_gate", shared::zBoolean()->setOptional()},
      {"depends_on", shared::zArray(shared::zString())->setOptional()},
      {"assigned_agent_id", shared::zString()->setOptional()},
      {"attempt_count", shared::zInteger()->setOptional()},
      {"result_summary", shared::zString()->setOptional()},
      {"review_summary", shared::zString()->setOptional()},
      {"files_to_read", shared::zArray(shared::zString())->setOptional()},
      {"files_to_touch", shared::zArray(shared::zString())->setOptional()},
      {"cwd", shared::zString()->setOptional()},
      {"verification_condition", shared::zString()->setOptional()},
      {"handoff_notes", shared::zString()->setOptional()},
      {"tasks",
       shared::zArray(shared::zObject({
           {"id", shared::zString()},
           {"title", shared::zString()},
           {"goal", shared::zString()},
           {"status",
            shared::zEnum({"Ready", "InProgress", "Implemented", "Verifying",
                           "Done", "Blocked", "Failed", "Cancelled"})
                ->setOptional()},
           {"notes", shared::zString()->setOptional()},
           {"verification_condition", shared::zString()->setOptional()},
           {"assigned_worker_id", shared::zString()->setOptional()},
       }))
           ->setOptional()},
  });
}

shared::ToolResult WorkTool::execute(const rapidjson::Value &input,
                                     shared::ToolContext &ctx) {
  try {
    const std::string action = actionOf(input);

    if (action == "CreatePlan") {
      requirePlanWriteAccess(ctx);
      const std::string threadId = requireCurrentThreadId(ctx);
      auto tm = makeThreadManager();

      shared::Plan plan;
      plan.threadId = threadId;
      plan.title = requireString(input, "title");
      plan.objective = requireString(input, "objective");
      plan.context = requireString(input, "context");
      plan.strategy = requireString(input, "strategy");
      if (input.HasMember("notes") && input["notes"].IsString()) {
        plan.notes = input["notes"].GetString();
      }

      const bool setActive =
          !input.HasMember("set_active") || input["set_active"].GetBool();
      if (input.HasMember("status") && input["status"].IsString()) {
        plan.status = parsePlanStatus(input["status"].GetString());
      } else {
        plan.status = setActive ? shared::PlanStatus::Active
                                : shared::PlanStatus::Draft;
      }

      const std::string planId = tm.createPlan(plan);
      const shared::Plan persistedPlan = tm.getPlan(threadId, planId);

      if (setActive) {
        auto metadata = tm.getMetadata(threadId);
        const bool activePlanChanged = metadata.activePlanId != planId;
        metadata.activePlanId = planId;
        tm.updateMetadata(threadId, metadata);
        if (activePlanChanged) {
          emitWorkEvent(shared::PlanCreated{threadId, persistedPlan});
          emitWorkEvent(
              shared::PlanActivated{threadId, persistedPlan.id, persistedPlan});
        } else {
          emitWorkEvent(shared::PlanCreated{threadId, persistedPlan});
        }
      } else {
        emitWorkEvent(shared::PlanCreated{threadId, persistedPlan});
      }

      rapidjson::Document doc;
      doc.SetObject();
      auto &alloc = doc.GetAllocator();
      doc.AddMember("plan_id", rapidjson::Value(planId.c_str(), alloc), alloc);
      std::string status = planStatusToString(persistedPlan.status);
      doc.AddMember("status", rapidjson::Value(status.c_str(), alloc), alloc);
      doc.AddMember("active", setActive, alloc);
      return shared::ToolResult::ok(doc);
    }

    if (action == "ListPlans") {
      requirePlanReadAccess(ctx);
      const std::string threadId = requireCurrentThreadId(ctx);
      auto tm = makeThreadManager();
      const auto metadata = tm.getMetadata(threadId);
      const auto plans = tm.listPlans(threadId);

      rapidjson::Document doc;
      doc.SetArray();
      auto &alloc = doc.GetAllocator();
      for (const auto &plan : plans) {
        doc.PushBack(makePlanSummary(plan, metadata.activePlanId == plan.id, alloc),
                     alloc);
      }
      return shared::ToolResult::ok(doc);
    }

    if (action == "GetPlan") {
      requirePlanReadAccess(ctx);
      const std::string threadId = requireCurrentThreadId(ctx);
      auto tm = makeThreadManager();
      const shared::Plan plan = tm.getPlan(threadId, requireString(input, "plan_id"));
      return shared::ToolResult::ok(shared::toJson(plan));
    }

    if (action == "UpdatePlan") {
      requirePlanWriteAccess(ctx);
      const std::string threadId = requireCurrentThreadId(ctx);
      auto tm = makeThreadManager();
      shared::Plan plan = tm.getPlan(threadId, requireString(input, "plan_id"));

      if (shouldTreatAsRequestedField(input, "title")) {
        plan.title = input["title"].GetString();
      }
      if (shouldTreatAsRequestedField(input, "objective")) {
        plan.objective = input["objective"].GetString();
      }
      if (shouldTreatAsRequestedField(input, "context")) {
        plan.context = input["context"].GetString();
      }
      if (shouldTreatAsRequestedField(input, "strategy")) {
        plan.strategy = input["strategy"].GetString();
      }
      if (shouldTreatAsRequestedField(input, "notes")) {
        plan.notes = input["notes"].GetString();
      }
      if (shouldTreatAsRequestedField(input, "status")) {
        plan.status = parsePlanStatus(input["status"].GetString());
      }

      tm.updatePlan(threadId, plan);
      const shared::Plan updated = tm.getPlan(threadId, plan.id);
      emitWorkEvent(shared::PlanUpdated{threadId, updated});

      rapidjson::Document doc;
      doc.SetObject();
      auto &alloc = doc.GetAllocator();
      doc.AddMember("plan_id", rapidjson::Value(updated.id.c_str(), alloc), alloc);
      std::string status = planStatusToString(updated.status);
      doc.AddMember("status", rapidjson::Value(status.c_str(), alloc), alloc);
      return shared::ToolResult::ok(doc);
    }

    if (action == "ActivatePlan") {
      requirePlanWriteAccess(ctx);
      const std::string threadId = requireCurrentThreadId(ctx);
      auto tm = makeThreadManager();
      shared::Plan plan = tm.getPlan(threadId, requireString(input, "plan_id"));

      auto metadata = tm.getMetadata(threadId);
      const bool activePlanChanged = metadata.activePlanId != plan.id;
      metadata.activePlanId = plan.id;
      tm.updateMetadata(threadId, metadata);

      const bool statusChanged = plan.status != shared::PlanStatus::Active;
      if (statusChanged) {
        plan.status = shared::PlanStatus::Active;
        tm.updatePlan(threadId, plan);
        plan = tm.getPlan(threadId, plan.id);
      }

      if (activePlanChanged || statusChanged) {
        emitWorkEvent(shared::PlanActivated{threadId, plan.id, plan});
      }

      rapidjson::Document doc;
      doc.SetObject();
      auto &alloc = doc.GetAllocator();
      doc.AddMember("plan_id", rapidjson::Value(plan.id.c_str(), alloc), alloc);
      std::string status = planStatusToString(plan.status);
      doc.AddMember("status", rapidjson::Value(status.c_str(), alloc), alloc);
      doc.AddMember("active", true, alloc);
      return shared::ToolResult::ok(doc);
    }

    if (action == "AddChunk") {
      requireChunkAddAccess(ctx);
      const std::string threadId = requireCurrentThreadId(ctx);
      auto tm = makeThreadManager();
      shared::Plan updatedPlan;
      shared::WorkChunk addedChunk;

      const std::string planId = requireString(input, "plan_id");

      updatedPlan = tm.mutatePlan(threadId, planId, [&](shared::Plan &plan) {
        shared::WorkChunk chunk;
        chunk.id = shared::StringUtil::generateUuid();
        chunk.title = requireString(input, "title");
        chunk.goal = requireString(input, "goal");
        chunk.context = requireString(input, "context");
        chunk.constraints = requireString(input, "constraints");
        chunk.completion = requireString(input, "completion");
        chunk.planningGate = input.HasMember("planning_gate") &&
                             input["planning_gate"].IsBool() &&
                             input["planning_gate"].GetBool();
        chunk.status =
            input.HasMember("status") && input["status"].IsString()
                ? parseChunkStatus(input["status"].GetString())
                : shared::WorkChunkStatus::Ready;
        chunk.dependsOn = parseStringArray(input, "depends_on");

        // V2 rich chunk spec fields
        chunk.filesToRead = parseStringArray(input, "files_to_read");
        chunk.filesToTouch = parseStringArray(input, "files_to_touch");
        chunk.cwd = shouldTreatAsRequestedField(input, "cwd") ? input["cwd"].GetString() : "";
        chunk.verificationCondition =
            shouldTreatAsRequestedField(input, "verification_condition")
                ? input["verification_condition"].GetString()
                : "";
        chunk.handoffNotes = shouldTreatAsRequestedField(input, "handoff_notes")
                                 ? input["handoff_notes"].GetString()
                                 : "";
        chunk.tasks = parseTaskArray(input, "tasks");

        chunk.createdAt = nowEpochMs();
        chunk.updatedAt = chunk.createdAt;
        for (auto &task : chunk.tasks) {
          task.createdAt = chunk.createdAt;
          task.updatedAt = chunk.updatedAt;
        }

        blockChunkIfDependenciesIncomplete(plan, chunk);
        plan.chunks.push_back(chunk);
        addedChunk = plan.chunks.back();
      });

      emitWorkEvent(shared::ChunkAdded{threadId, updatedPlan.id, addedChunk});
      emitWorkEvent(shared::PlanUpdated{threadId, updatedPlan});

      rapidjson::Document doc;
      doc.SetObject();
      auto &alloc = doc.GetAllocator();
      doc.AddMember("chunk_id", rapidjson::Value(addedChunk.id.c_str(), alloc),
                    alloc);
      std::string status = chunkStatusToString(addedChunk.status);
      doc.AddMember("status", rapidjson::Value(status.c_str(), alloc), alloc);
      return shared::ToolResult::ok(doc);
    }

    if (action == "ListChunks") {
      requireChunkReadAccess(ctx);
      const std::string threadId = requireCurrentThreadId(ctx);
      auto tm = makeThreadManager();
      const shared::Plan plan = tm.getPlan(threadId, requireString(input, "plan_id"));

      rapidjson::Document doc;
      doc.SetArray();
      auto &alloc = doc.GetAllocator();
      for (const auto &chunk : plan.chunks) {
        doc.PushBack(makeChunkSummary(chunk, alloc), alloc);
      }
      return shared::ToolResult::ok(doc);
    }

    if (action == "GetChunk") {
      requireChunkReadAccess(ctx);
      const std::string threadId = requireCurrentThreadId(ctx);
      auto tm = makeThreadManager();
      const shared::Plan plan = tm.getPlan(threadId, requireString(input, "plan_id"));
      const std::string chunkId = requireString(input, "chunk_id");
      const auto &chunk = requireChunk(plan, chunkId);
      return shared::ToolResult::ok(shared::toJson(chunk));
    }

    if (action == "UpdateChunk") {
      const std::string threadId = requireCurrentThreadId(ctx);
      auto tm = makeThreadManager();

      const std::string planId = requireString(input, "plan_id");
      const std::string chunkId = requireString(input, "chunk_id");

      shared::WorkChunk originalChunk;
      shared::WorkChunk updatedChunk;

      auto updatedPlan = tm.mutatePlan(threadId, planId, [&](shared::Plan &plan) {
        auto &chunk = requireChunk(plan, chunkId);
        requireChunkUpdateAccess(input, ctx, threadId, plan, chunk);
        originalChunk = chunk;

        const bool statusWasProvided = shouldApplyOptionalField(input, "status");
        const bool assignedAgentWasMentioned = input.HasMember("assigned_agent_id");
        const bool assignedAgentEmpty =
            assignedAgentWasMentioned && input["assigned_agent_id"].IsString() &&
            shared::StringUtil::trim(
                std::string_view(input["assigned_agent_id"].GetString()))
                .empty();
        const bool assignedAgentWasProvided =
            assignedAgentWasMentioned &&
            (!assignedAgentEmpty || !hasMeaningfulMutationOtherThanAssignment(input));

        const auto effectiveStatus =
            statusWasProvided ? parseChunkStatus(input["status"].GetString())
                             : chunk.status;

        if (assignedAgentWasProvided) {
          const auto status = effectiveStatus;
          const bool allowReassign = status == shared::WorkChunkStatus::Ready ||
                                     status == shared::WorkChunkStatus::Failed ||
                                     status == shared::WorkChunkStatus::Blocked;

          const std::string newAgentId =
              input["assigned_agent_id"].GetString();
          if (!allowReassign && newAgentId != chunk.assignedAgentId) {
            throw std::runtime_error(
                "assigned_agent_id may be updated only when chunk status is "
                "Ready, Failed, or Blocked");
          }
          if (!newAgentId.empty()) {
            validateExecutorAssignmentInvariant(tm, threadId, plan.id, chunk.id,
                                                newAgentId);
          }
          chunk.assignedAgentId = newAgentId;
        }

        if (shouldApplyOptionalField(input, "title")) {
          chunk.title = input["title"].GetString();
        }
        if (shouldApplyOptionalField(input, "goal")) {
          chunk.goal = input["goal"].GetString();
        }
        if (shouldApplyOptionalField(input, "context")) {
          chunk.context = input["context"].GetString();
        }
        if (shouldApplyOptionalField(input, "constraints")) {
          chunk.constraints = input["constraints"].GetString();
        }
        if (shouldApplyOptionalField(input, "completion")) {
          chunk.completion = input["completion"].GetString();
        }
        if (input.HasMember("planning_gate") && input["planning_gate"].IsBool()) {
          chunk.planningGate = input["planning_gate"].GetBool();
        }
        if (statusWasProvided) {
          chunk.status = effectiveStatus;
        }
        if (shouldApplyOptionalField(input, "depends_on")) {
          chunk.dependsOn = parseStringArray(input, "depends_on");
        }
        if (input.HasMember("attempt_count") && input["attempt_count"].IsInt()) {
          chunk.attemptCount = input["attempt_count"].GetInt();
        }
        if (shouldApplyOptionalField(input, "result_summary")) {
          chunk.resultSummary = input["result_summary"].GetString();
        }
        if (shouldApplyOptionalField(input, "review_summary")) {
          chunk.reviewSummary = input["review_summary"].GetString();
        }

        // V2 rich chunk spec fields
        if (shouldApplyOptionalField(input, "files_to_read")) {
          chunk.filesToRead = parseStringArray(input, "files_to_read");
        }
        if (shouldApplyOptionalField(input, "files_to_touch")) {
          chunk.filesToTouch = parseStringArray(input, "files_to_touch");
        }
        if (shouldApplyOptionalField(input, "cwd")) {
          chunk.cwd = input["cwd"].GetString();
        }
        if (shouldApplyOptionalField(input, "verification_condition")) {
          chunk.verificationCondition =
              input["verification_condition"].GetString();
        }
        if (shouldApplyOptionalField(input, "handoff_notes")) {
          chunk.handoffNotes = input["handoff_notes"].GetString();
        }
        if (shouldApplyOptionalField(input, "tasks")) {
          chunk.tasks = parseTaskArray(input, "tasks");
          for (auto &task : chunk.tasks) {
            if (task.createdAt == 0) {
              task.createdAt = nowEpochMs();
            }
            task.updatedAt = nowEpochMs();
          }
        }

        if (statusWasProvided && chunk.status == shared::WorkChunkStatus::Done) {
          if (roleForContext(ctx) != WorkAgentRole::Lead) {
            throw std::runtime_error(
                "Only the lead may mark a chunk Done after review");
          }
          if (!chunkDependenciesDone(plan, chunk)) {
            throw std::runtime_error("Chunk '" + chunk.id +
                                     "' cannot be marked Done until "
                                     "dependencies are Done");
          }
          if (shared::StringUtil::trim(chunk.reviewSummary).empty()) {
            throw std::runtime_error("Chunk '" + chunk.id +
                                     "' cannot be marked Done without "
                                     "review_summary acceptance evidence");
          }
          unblockDependentChunks(plan, chunk.id);
        }

        if (statusWasProvided || shouldApplyOptionalField(input, "depends_on")) {
          blockChunkIfDependenciesIncomplete(plan, chunk);
          unblockChunkIfDependenciesMet(plan, chunk);
        }

        reconcileChunkDependencies(plan);

        chunk.updatedAt = nowEpochMs();
        updatedChunk = chunk;
      });

      emitWorkEvent(shared::ChunkUpdated{threadId, updatedPlan.id, updatedChunk});
      emitWorkEvent(shared::PlanUpdated{threadId, updatedPlan});

      if (originalChunk.assignedAgentId != updatedChunk.assignedAgentId) {
        emitWorkEvent(shared::ChunkAssigned{threadId, updatedPlan.id,
                                            updatedChunk.id,
                                            updatedChunk.assignedAgentId,
                                            updatedChunk});
      }
      if (originalChunk.status != updatedChunk.status) {
        emitWorkEvent(shared::ChunkStatusChanged{
            threadId, updatedPlan.id, updatedChunk.id, originalChunk.status,
            updatedChunk.status, updatedChunk});
      }

      rapidjson::Document doc;
      doc.SetObject();
      auto &alloc = doc.GetAllocator();
      doc.AddMember("chunk_id", rapidjson::Value(updatedChunk.id.c_str(), alloc),
                    alloc);
      std::string status = chunkStatusToString(updatedChunk.status);
      doc.AddMember("status", rapidjson::Value(status.c_str(), alloc), alloc);
      return shared::ToolResult::ok(doc);
    }

    if (action == "ReadyChunk") {
      requireChunkReadAccess(ctx);
      const std::string threadId = requireCurrentThreadId(ctx);
      auto tm = makeThreadManager();
      const shared::Plan plan = tm.getPlan(threadId, requireString(input, "plan_id"));

      rapidjson::Document doc;
      doc.SetArray();
      auto &alloc = doc.GetAllocator();
      for (const auto &chunk : plan.chunks) {
        if (chunkReadyForExecution(plan, chunk)) {
          doc.PushBack(makeChunkSummary(chunk, alloc), alloc);
        }
      }
      return shared::ToolResult::ok(doc);
    }

    return shared::ToolResult::fail("Unknown Work.action");
  } catch (const std::exception &e) {
    return shared::ToolResult::fail(e.what());
  }
}

} // namespace firmius::core

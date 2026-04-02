#include "tools/ChunkUpdateTool.hpp"
#include "tools/WorkToolCommon.hpp"
#include "utils/StringUtil.hpp"

namespace firmius::core {

namespace {

bool shouldApplyOptionalField(const rapidjson::Value &input, const char *field) {
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

} // namespace

shared::ToolMetadata ChunkUpdateTool::getMetadata() const {
  return {"chunk_update", "Update chunk fields in a plan",
          shared::ToolScope::Semantic};
}

std::shared_ptr<shared::JSONSchema> ChunkUpdateTool::getSchema() const {
  return zObject(
             {
                 {"plan_id", zString()},
                 {"chunk_id", zString()},
                 {"title", zString()->setOptional()},
                 {"goal", zString()->setOptional()},
                 {"context", zString()->setOptional()},
                 {"constraints", zString()->setOptional()},
                 {"completion", zString()->setOptional()},
                 {"planning_gate", zBoolean()->setOptional()},
                 {"status",
                  zEnum({"Ready", "InProgress", "Implemented", "Verifying",
                         "Done", "Blocked", "Failed", "Cancelled"})
                      ->setOptional()},
                 {"depends_on", zArray(zString())->setOptional()},
                 {"assigned_agent_id", zString()->setOptional()},
                 {"attempt_count", zInteger()->setOptional()},
                 {"result_summary", zString()->setOptional()},
                 {"review_summary", zString()->setOptional()},
                 // V2 rich chunk spec fields (all optional)
                 {"files_to_read", zArray(zString())->setOptional()},
                 {"files_to_touch", zArray(zString())->setOptional()},
                 {"cwd", zString()->setOptional()},
                 {"verification_condition", zString()->setOptional()},
                 {"handoff_notes", zString()->setOptional()},
                 {"tasks",
                  zArray(
                      zObject({
                          {"id", zString()},
                          {"title", zString()},
                          {"goal", zString()},
                          {"status", zEnum({"Ready", "InProgress",
                                            "Implemented", "Verifying", "Done",
                                            "Blocked", "Failed", "Cancelled"})
                                         ->setOptional()},
                          {"notes", zString()->setOptional()},
                          {"verification_condition", zString()->setOptional()},
                          {"assigned_worker_id", zString()->setOptional()},
                      }))
                      ->setOptional()},
             })
      ->required({"plan_id", "chunk_id"});
}

shared::ToolResult ChunkUpdateTool::execute(const rapidjson::Value &input,
                                            shared::ToolContext &ctx) {
  try {
    const std::string threadId = worktools::requireCurrentThreadId(ctx);
    auto tm = worktools::makeThreadManager();
    shared::WorkChunk originalChunk;
    shared::WorkChunk updatedChunk;
    const bool statusWasProvided = shouldApplyOptionalField(input, "status");
    const bool assignedAgentWasMentioned = input.HasMember("assigned_agent_id");
    const bool assignedAgentEmpty =
        assignedAgentWasMentioned &&
        shared::StringUtil::trim(
            std::string_view(input["assigned_agent_id"].GetString()))
            .empty();
    const bool assignedAgentWasProvided =
        assignedAgentWasMentioned &&
        (!assignedAgentEmpty || !hasMeaningfulMutationOtherThanAssignment(input));

    auto updatedPlan = tm.mutatePlan(
        threadId, input["plan_id"].GetString(), [&](shared::Plan &plan) {
          auto &chunk =
              worktools::requireChunk(plan, input["chunk_id"].GetString());
          worktools::requireChunkUpdateAccess(input, ctx, threadId, plan,
                                              chunk);
          originalChunk = chunk;
          const bool dependsOnWasProvided =
              shouldApplyOptionalField(input, "depends_on");

          const auto effectiveStatus =
              statusWasProvided
                  ? worktools::parseChunkStatus(input["status"].GetString())
                  : chunk.status;

          if (assignedAgentWasProvided) {
            const auto status = effectiveStatus;
            const bool allowReassign =
                status == shared::WorkChunkStatus::Ready ||
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
              worktools::validateExecutorAssignmentInvariant(
                  tm, threadId, plan.id, chunk.id, newAgentId);
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
          if (input.HasMember("planning_gate")) {
            chunk.planningGate = input["planning_gate"].GetBool();
          }
          if (statusWasProvided) {
            chunk.status = effectiveStatus;
          }
          if (dependsOnWasProvided) {
            chunk.dependsOn = worktools::parseStringArray(input, "depends_on");
          }
          if (input.HasMember("attempt_count")) {
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
            chunk.filesToRead =
                worktools::parseStringArray(input, "files_to_read");
          }
          if (shouldApplyOptionalField(input, "files_to_touch")) {
            chunk.filesToTouch =
                worktools::parseStringArray(input, "files_to_touch");
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
            chunk.tasks = worktools::parseTaskArray(input, "tasks");
            for (auto &task : chunk.tasks) {
              if (task.createdAt == 0) {
                task.createdAt = worktools::nowEpochMs();
              }
              task.updatedAt = worktools::nowEpochMs();
            }
          }

          if (statusWasProvided &&
              chunk.status == shared::WorkChunkStatus::Done) {
            if (worktools::roleForContext(ctx) !=
                worktools::WorkAgentRole::Lead) {
              throw std::runtime_error(
                  "Only the lead may mark a chunk Done after review");
            }
            if (!worktools::chunkDependenciesDone(plan, chunk)) {
              throw std::runtime_error("Chunk '" + chunk.id +
                                       "' cannot be marked Done until "
                                       "dependencies are Done");
            }
            if (shared::StringUtil::trim(chunk.reviewSummary).empty()) {
              throw std::runtime_error("Chunk '" + chunk.id +
                                       "' cannot be marked Done without "
                                       "review_summary acceptance evidence");
            }
            worktools::unblockDependentChunks(plan, chunk.id);
          }

          if (statusWasProvided || dependsOnWasProvided) {
            worktools::blockChunkIfDependenciesIncomplete(plan, chunk);
            worktools::unblockChunkIfDependenciesMet(plan, chunk);
          }

          // Always reconcile dependencies after any mutation
          worktools::reconcileChunkDependencies(plan);

          chunk.updatedAt = worktools::nowEpochMs();
          updatedChunk = chunk;
        });

    worktools::emitWorkEvent(
        shared::ChunkUpdated{threadId, updatedPlan.id, updatedChunk});
    worktools::emitWorkEvent(shared::PlanUpdated{threadId, updatedPlan});
    if (originalChunk.assignedAgentId != updatedChunk.assignedAgentId) {
      worktools::emitWorkEvent(
          shared::ChunkAssigned{threadId, updatedPlan.id, updatedChunk.id,
                                updatedChunk.assignedAgentId, updatedChunk});
    }
    if (originalChunk.status != updatedChunk.status) {
      worktools::emitWorkEvent(shared::ChunkStatusChanged{
          threadId, updatedPlan.id, updatedChunk.id, originalChunk.status,
          updatedChunk.status, updatedChunk});
    }
    rapidjson::Document doc;
    doc.SetObject();
    auto &alloc = doc.GetAllocator();
    doc.AddMember("chunk_id", rapidjson::Value(updatedChunk.id.c_str(), alloc),
                  alloc);
    std::string status = worktools::chunkStatusToString(updatedChunk.status);
    doc.AddMember("status", rapidjson::Value(status.c_str(), alloc), alloc);
    return shared::ToolResult::ok(doc);
  } catch (const std::exception &e) {
    return shared::ToolResult::fail(e.what());
  }
}

} // namespace firmius::core

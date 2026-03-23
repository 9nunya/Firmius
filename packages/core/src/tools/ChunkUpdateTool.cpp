#include "tools/ChunkUpdateTool.hpp"
#include "tools/WorkToolCommon.hpp"
#include "utils/StringUtil.hpp"

namespace firmius::core {

shared::ToolMetadata ChunkUpdateTool::getMetadata() const {
  return {"chunk_update", "Update chunk fields in a plan",
          shared::ToolScope::Semantic};
}

std::shared_ptr<shared::JSONSchema> ChunkUpdateTool::getSchema() const {
  return zObject({
             {"plan_id", zString()},
             {"chunk_id", zString()},
             {"title", zString()->setOptional()},
             {"goal", zString()->setOptional()},
             {"context", zString()->setOptional()},
             {"constraints", zString()->setOptional()},
             {"completion", zString()->setOptional()},
             {"planning_gate", zBoolean()->setOptional()},
             {"status",
              zEnum({"Ready", "InProgress", "Implemented",
                     "Verifying", "Done", "Blocked", "Failed", "Cancelled"})
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
              zArray(zObject({
                  {"id", zString()},
                  {"title", zString()},
                  {"goal", zString()},
                  {"status",
                   zEnum({"Ready", "InProgress", "Implemented", "Verifying",
                          "Done", "Blocked", "Failed", "Cancelled"})
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
    const bool statusWasProvided = input.HasMember("status");
    const bool assignedAgentWasProvided = input.HasMember("assigned_agent_id");

    auto updatedPlan = tm.mutatePlan(
        threadId, input["plan_id"].GetString(), [&](shared::Plan &plan) {
          auto &chunk =
              worktools::requireChunk(plan, input["chunk_id"].GetString());
          worktools::requireChunkUpdateAccess(input, ctx, threadId, plan,
                                              chunk);
          originalChunk = chunk;
          const bool dependsOnWasProvided = input.HasMember("depends_on");

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
            if (!allowReassign) {
              throw std::runtime_error(
                  "assigned_agent_id may be updated only when chunk status is "
                  "Ready, Failed, or Blocked");
            }
            const std::string newAgentId =
                input["assigned_agent_id"].GetString();
            if (!newAgentId.empty()) {
              worktools::validateExecutorAssignmentInvariant(
                  tm, threadId, plan.id, chunk.id, newAgentId);
            }
            chunk.assignedAgentId = newAgentId;
          }

          if (input.HasMember("title")) {
            chunk.title = input["title"].GetString();
          }
          if (input.HasMember("goal")) {
            chunk.goal = input["goal"].GetString();
          }
          if (input.HasMember("context")) {
            chunk.context = input["context"].GetString();
          }
          if (input.HasMember("constraints")) {
            chunk.constraints = input["constraints"].GetString();
          }
          if (input.HasMember("completion")) {
            chunk.completion = input["completion"].GetString();
          }
          if (input.HasMember("planning_gate")) {
            chunk.planningGate = input["planning_gate"].GetBool();
          }
          if (input.HasMember("status")) {
            chunk.status = effectiveStatus;
          }
          if (input.HasMember("depends_on")) {
            chunk.dependsOn = worktools::parseStringArray(input, "depends_on");
          }
          if (input.HasMember("attempt_count")) {
            chunk.attemptCount = input["attempt_count"].GetInt();
          }
          if (input.HasMember("result_summary")) {
            chunk.resultSummary = input["result_summary"].GetString();
          }
          if (input.HasMember("review_summary")) {
            chunk.reviewSummary = input["review_summary"].GetString();
          }
          // V2 rich chunk spec fields
          if (input.HasMember("files_to_read")) {
            chunk.filesToRead = worktools::parseStringArray(input, "files_to_read");
          }
          if (input.HasMember("files_to_touch")) {
            chunk.filesToTouch = worktools::parseStringArray(input, "files_to_touch");
          }
          if (input.HasMember("cwd")) {
            chunk.cwd = input["cwd"].GetString();
          }
          if (input.HasMember("verification_condition")) {
            chunk.verificationCondition = input["verification_condition"].GetString();
          }
          if (input.HasMember("handoff_notes")) {
            chunk.handoffNotes = input["handoff_notes"].GetString();
          }
          if (input.HasMember("tasks")) {
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
          }

          if (statusWasProvided || dependsOnWasProvided) {
            worktools::blockChunkIfDependenciesIncomplete(plan, chunk);
          }

          chunk.updatedAt = worktools::nowEpochMs();
          updatedChunk = chunk;
        });

    worktools::emitWorkEvent(
        shared::ChunkUpdated{threadId, updatedPlan.id, updatedChunk});
    if (originalChunk.assignedAgentId != updatedChunk.assignedAgentId) {
      worktools::emitWorkEvent(shared::ChunkAssigned{
          threadId, updatedPlan.id, updatedChunk.id,
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
    doc.AddMember("chunk_id",
                  rapidjson::Value(updatedChunk.id.c_str(), alloc), alloc);
    std::string status = worktools::chunkStatusToString(updatedChunk.status);
    doc.AddMember("status", rapidjson::Value(status.c_str(), alloc), alloc);
    return shared::ToolResult::ok(doc);
  } catch (const std::exception &e) {
    return shared::ToolResult::fail(e.what());
  }
}

} // namespace firmius::core

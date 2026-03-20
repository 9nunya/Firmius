#include "tools/ChunkAddTool.hpp"
#include "tools/WorkToolCommon.hpp"
#include "utils/StringUtil.hpp"

namespace firmius::core {

shared::ToolMetadata ChunkAddTool::getMetadata() const {
  return {"chunk_add", "Add a chunk to a plan",
          shared::ToolScope::ChunkWrite};
}

std::shared_ptr<shared::JSONSchema> ChunkAddTool::getSchema() const {
  return zObject({
             {"plan_id", zString()},
             {"title", zString()},
             {"goal", zString()},
             {"context", zString()},
             {"constraints", zString()},
             {"completion", zString()},
             {"planning_gate", zBoolean()->setOptional()},
             {"status",
              zEnum({"Ready", "InProgress", "Implemented",
                     "Verifying", "Done", "Blocked", "Failed", "Cancelled"})
                  ->setOptional()},
             {"depends_on", zArray(zString())->setOptional()},
             // V2 rich chunk spec fields (all optional)
             {"files_to_read", zArray(zString())->setOptional()},
             {"files_to_touch", zArray(zString())->setOptional()},
             {"cwd", zString()->setOptional()},
             {"verification_condition", zString()->setOptional()},
             {"handoff_notes", zString()->setOptional()},
         })
      ->required(
          {"plan_id", "title", "goal", "context", "constraints", "completion"});
}

shared::ToolResult ChunkAddTool::execute(const rapidjson::Value &input,
                                         shared::ToolContext &ctx) {
  try {
    worktools::requireChunkAddAccess(ctx);
    const std::string threadId = worktools::requireCurrentThreadId(ctx);
    auto tm = worktools::makeThreadManager();
    shared::Plan updatedPlan;
    shared::WorkChunk addedChunk;

    updatedPlan = tm.mutatePlan(
        threadId, input["plan_id"].GetString(), [&](shared::Plan &plan) {
          shared::WorkChunk chunk;
          chunk.id = shared::StringUtil::generateUuid();
          chunk.title = input["title"].GetString();
          chunk.goal = input["goal"].GetString();
          chunk.context = input["context"].GetString();
          chunk.constraints = input["constraints"].GetString();
          chunk.completion = input["completion"].GetString();
          chunk.planningGate = input.HasMember("planning_gate") &&
                               input["planning_gate"].GetBool();
          chunk.status =
              input.HasMember("status")
                  ? worktools::parseChunkStatus(input["status"].GetString())
                  : shared::WorkChunkStatus::Ready;
          chunk.dependsOn = worktools::parseStringArray(input, "depends_on");
          
          // V2 rich chunk spec fields
          chunk.filesToRead = worktools::parseStringArray(input, "files_to_read");
          chunk.filesToTouch = worktools::parseStringArray(input, "files_to_touch");
          chunk.cwd = input.HasMember("cwd") ? input["cwd"].GetString() : "";
          chunk.verificationCondition = input.HasMember("verification_condition")
              ? input["verification_condition"].GetString() : "";
          chunk.handoffNotes = input.HasMember("handoff_notes")
              ? input["handoff_notes"].GetString() : "";
          
          chunk.createdAt = worktools::nowEpochMs();
          chunk.updatedAt = chunk.createdAt;
          worktools::blockChunkIfDependenciesIncomplete(plan, chunk);
          plan.chunks.push_back(chunk);
          addedChunk = plan.chunks.back();
        });
    worktools::emitWorkEvent(
        shared::ChunkAdded{threadId, updatedPlan.id, addedChunk});

    rapidjson::Document doc;
    doc.SetObject();
    auto &alloc = doc.GetAllocator();
    doc.AddMember("chunk_id", rapidjson::Value(addedChunk.id.c_str(), alloc),
                  alloc);
    std::string status = worktools::chunkStatusToString(addedChunk.status);
    doc.AddMember("status", rapidjson::Value(status.c_str(), alloc), alloc);
    return shared::ToolResult::ok(doc);
  } catch (const std::exception &e) {
    return shared::ToolResult::fail(e.what());
  }
}

} // namespace firmius::core

#include "tools/ChunkUpdateTool.hpp"
#include "tools/WorkToolCommon.hpp"

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
             {"status",
              zEnum({"Draft", "Ready", "InProgress", "Implemented",
                     "Verifying", "Done", "Blocked", "Failed", "Cancelled"})
                  ->setOptional()},
             {"depends_on", zArray(zString())->setOptional()},
             {"attempt_count", zInteger()->setOptional()},
             {"result_summary", zString()->setOptional()},
             {"review_summary", zString()->setOptional()},
         })
      ->required({"plan_id", "chunk_id"});
}

shared::ToolResult ChunkUpdateTool::execute(const rapidjson::Value &input,
                                            shared::ToolContext &ctx) {
  try {
    const std::string threadId = worktools::requireCurrentThreadId(ctx);
    auto tm = worktools::makeThreadManager();
    shared::Plan plan =
        worktools::loadPlan(tm, threadId, input["plan_id"].GetString());
    auto &chunk = worktools::requireChunk(plan, input["chunk_id"].GetString());
    worktools::requireChunkUpdateAccess(input, ctx, threadId, plan, chunk);
    const shared::WorkChunk originalChunk = chunk;

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
    if (input.HasMember("status")) {
      chunk.status = worktools::parseChunkStatus(input["status"].GetString());
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
    chunk.updatedAt = worktools::nowEpochMs();

    tm.updatePlan(threadId, plan);
    const shared::WorkChunk updatedChunk = chunk;
    worktools::emitWorkEvent(
        shared::ChunkUpdated{threadId, plan.id, updatedChunk});
    if (originalChunk.status != updatedChunk.status) {
      worktools::emitWorkEvent(shared::ChunkStatusChanged{
          threadId, plan.id, updatedChunk.id, originalChunk.status,
          updatedChunk.status, updatedChunk});
    }
    rapidjson::Document doc;
    doc.SetObject();
    auto &alloc = doc.GetAllocator();
    doc.AddMember("chunk_id", rapidjson::Value(chunk.id.c_str(), alloc), alloc);
    std::string status = worktools::chunkStatusToString(chunk.status);
    doc.AddMember("status", rapidjson::Value(status.c_str(), alloc), alloc);
    return shared::ToolResult::ok(doc);
  } catch (const std::exception &e) {
    return shared::ToolResult::fail(e.what());
  }
}

} // namespace firmius::core

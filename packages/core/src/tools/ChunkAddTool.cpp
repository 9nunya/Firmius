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
             {"status",
              zEnum({"Draft", "Ready", "InProgress", "Implemented",
                     "Verifying", "Done", "Blocked", "Failed", "Cancelled"})
                  ->setOptional()},
             {"depends_on", zArray(zString())->setOptional()},
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
    shared::Plan plan =
        worktools::loadPlan(tm, threadId, input["plan_id"].GetString());

    shared::WorkChunk chunk;
    chunk.id = shared::StringUtil::generateUuid();
    chunk.title = input["title"].GetString();
    chunk.goal = input["goal"].GetString();
    chunk.context = input["context"].GetString();
    chunk.constraints = input["constraints"].GetString();
    chunk.completion = input["completion"].GetString();
    chunk.status = input.HasMember("status")
                       ? worktools::parseChunkStatus(input["status"].GetString())
                       : shared::WorkChunkStatus::Ready;
    chunk.dependsOn = worktools::parseStringArray(input, "depends_on");
    chunk.createdAt = worktools::nowEpochMs();
    chunk.updatedAt = chunk.createdAt;

    plan.chunks.push_back(chunk);
    tm.updatePlan(threadId, plan);
    worktools::emitWorkEvent(shared::ChunkAdded{threadId, plan.id, chunk});

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

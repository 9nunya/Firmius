#include "tools/ChunkGetTool.hpp"
#include "tools/WorkToolCommon.hpp"

namespace firmius::core {

shared::ToolMetadata ChunkGetTool::getMetadata() const {
  return {"chunk_get", "Get a full chunk from a plan",
          shared::ToolScope::ChunkRead};
}

std::shared_ptr<shared::JSONSchema> ChunkGetTool::getSchema() const {
  return zObject({{"plan_id", zString()}, {"chunk_id", zString()}})
      ->required({"plan_id", "chunk_id"});
}

shared::ToolResult ChunkGetTool::execute(const rapidjson::Value &input,
                                         shared::ToolContext &ctx) {
  try {
    worktools::requireChunkReadAccess(ctx);
    const std::string threadId = worktools::requireCurrentThreadId(ctx);
    auto tm = worktools::makeThreadManager();
    const shared::Plan plan =
        worktools::loadPlan(tm, threadId, input["plan_id"].GetString());
    return shared::ToolResult::ok(shared::toJson(
        worktools::requireChunk(plan, input["chunk_id"].GetString())));
  } catch (const std::exception &e) {
    return shared::ToolResult::fail(e.what());
  }
}

} // namespace firmius::core

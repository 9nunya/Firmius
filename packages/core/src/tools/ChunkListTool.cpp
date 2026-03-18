#include "tools/ChunkListTool.hpp"
#include "tools/WorkToolCommon.hpp"

namespace firmius::core {

shared::ToolMetadata ChunkListTool::getMetadata() const {
  return {"chunk_list", "List chunk summaries for a plan",
          shared::ToolScope::ChunkRead};
}

std::shared_ptr<shared::JSONSchema> ChunkListTool::getSchema() const {
  return zObject({{"plan_id", zString()}})->required({"plan_id"});
}

shared::ToolResult ChunkListTool::execute(const rapidjson::Value &input,
                                          shared::ToolContext &ctx) {
  try {
    worktools::requireChunkReadAccess(ctx);
    const std::string threadId = worktools::requireCurrentThreadId(ctx);
    auto tm = worktools::makeThreadManager();
    const shared::Plan plan =
        worktools::loadPlan(tm, threadId, input["plan_id"].GetString());

    rapidjson::Document doc;
    doc.SetArray();
    auto &alloc = doc.GetAllocator();
    for (const auto &chunk : plan.chunks) {
      doc.PushBack(worktools::makeChunkSummary(chunk, alloc), alloc);
    }
    return shared::ToolResult::ok(doc);
  } catch (const std::exception &e) {
    return shared::ToolResult::fail(e.what());
  }
}

} // namespace firmius::core

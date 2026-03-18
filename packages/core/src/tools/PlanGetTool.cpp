#include "tools/PlanGetTool.hpp"
#include "tools/WorkToolCommon.hpp"

namespace firmius::core {

shared::ToolMetadata PlanGetTool::getMetadata() const {
  return {"plan_get", "Get a full plan with embedded chunks",
          shared::ToolScope::PlanRead};
}

std::shared_ptr<shared::JSONSchema> PlanGetTool::getSchema() const {
  return zObject({{"plan_id", zString()}})->required({"plan_id"});
}

shared::ToolResult PlanGetTool::execute(const rapidjson::Value &input,
                                        shared::ToolContext &ctx) {
  try {
    worktools::requirePlanReadAccess(ctx);
    const std::string threadId = worktools::requireCurrentThreadId(ctx);
    auto tm = worktools::makeThreadManager();
    return shared::ToolResult::ok(
        shared::toJson(worktools::loadPlan(tm, threadId, input["plan_id"].GetString())));
  } catch (const std::exception &e) {
    return shared::ToolResult::fail(e.what());
  }
}

} // namespace firmius::core

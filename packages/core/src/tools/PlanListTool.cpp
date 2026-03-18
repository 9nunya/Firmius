#include "tools/PlanListTool.hpp"
#include "tools/WorkToolCommon.hpp"

namespace firmius::core {

shared::ToolMetadata PlanListTool::getMetadata() const {
  return {"plan_list", "List plans for the current thread",
          shared::ToolScope::PlanRead};
}

std::shared_ptr<shared::JSONSchema> PlanListTool::getSchema() const {
  return zObject();
}

shared::ToolResult PlanListTool::execute(const rapidjson::Value &,
                                         shared::ToolContext &ctx) {
  try {
    worktools::requirePlanReadAccess(ctx);
    const std::string threadId = worktools::requireCurrentThreadId(ctx);
    auto tm = worktools::makeThreadManager();
    const auto metadata = tm.getMetadata(threadId);
    const auto plans = tm.listPlans(threadId);

    rapidjson::Document doc;
    doc.SetArray();
    auto &alloc = doc.GetAllocator();
    for (const auto &plan : plans) {
      doc.PushBack(worktools::makePlanSummary(
                       plan, metadata.activePlanId == plan.id, alloc),
                   alloc);
    }
    return shared::ToolResult::ok(doc);
  } catch (const std::exception &e) {
    return shared::ToolResult::fail(e.what());
  }
}

} // namespace firmius::core

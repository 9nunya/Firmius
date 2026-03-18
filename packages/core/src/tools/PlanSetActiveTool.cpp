#include "tools/PlanSetActiveTool.hpp"
#include "tools/WorkToolCommon.hpp"

namespace firmius::core {

shared::ToolMetadata PlanSetActiveTool::getMetadata() const {
  return {"plan_set_active", "Set the active plan for the current thread",
          shared::ToolScope::PlanWrite};
}

std::shared_ptr<shared::JSONSchema> PlanSetActiveTool::getSchema() const {
  return zObject({{"plan_id", zString()}})->required({"plan_id"});
}

shared::ToolResult PlanSetActiveTool::execute(const rapidjson::Value &input,
                                              shared::ToolContext &ctx) {
  try {
    worktools::requirePlanWriteAccess(ctx);
    const std::string threadId = worktools::requireCurrentThreadId(ctx);
    auto tm = worktools::makeThreadManager();
    shared::Plan plan =
        worktools::loadPlan(tm, threadId, input["plan_id"].GetString());

    auto metadata = tm.getMetadata(threadId);
    const bool activePlanChanged = metadata.activePlanId != plan.id;
    metadata.activePlanId = plan.id;
    tm.updateMetadata(threadId, metadata);

    const bool statusChanged = plan.status != shared::PlanStatus::Active;
    if (plan.status != shared::PlanStatus::Active) {
      plan.status = shared::PlanStatus::Active;
      tm.updatePlan(threadId, plan);
      plan = tm.getPlan(threadId, plan.id);
    }

    if (activePlanChanged || statusChanged) {
      worktools::emitWorkEvent(
          shared::PlanActivated{threadId, plan.id, plan});
    }

    rapidjson::Document doc;
    doc.SetObject();
    auto &alloc = doc.GetAllocator();
    doc.AddMember("plan_id", rapidjson::Value(plan.id.c_str(), alloc), alloc);
    std::string status = worktools::planStatusToString(plan.status);
    doc.AddMember("status", rapidjson::Value(status.c_str(), alloc), alloc);
    doc.AddMember("active", true, alloc);
    return shared::ToolResult::ok(doc);
  } catch (const std::exception &e) {
    return shared::ToolResult::fail(e.what());
  }
}

} // namespace firmius::core

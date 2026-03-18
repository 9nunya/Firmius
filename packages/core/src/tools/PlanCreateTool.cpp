#include "tools/PlanCreateTool.hpp"
#include "tools/WorkToolCommon.hpp"

namespace firmius::core {

shared::ToolMetadata PlanCreateTool::getMetadata() const {
  return {"plan_create", "Create a plan in the current thread",
          shared::ToolScope::PlanWrite};
}

std::shared_ptr<shared::JSONSchema> PlanCreateTool::getSchema() const {
  return zObject({
             {"title", zString()},
             {"objective", zString()},
             {"context", zString()},
             {"strategy", zString()},
             {"notes", zString()->setOptional()},
             {"status",
              zEnum({"Draft", "Active", "Paused", "Done", "Abandoned"})
                  ->setOptional()},
             {"set_active", zBoolean()->setOptional()},
         })
      ->required({"title", "objective", "context", "strategy"});
}

shared::ToolResult PlanCreateTool::execute(const rapidjson::Value &input,
                                           shared::ToolContext &ctx) {
  try {
    worktools::requirePlanWriteAccess(ctx);
    const std::string threadId = worktools::requireCurrentThreadId(ctx);
    auto tm = worktools::makeThreadManager();

    shared::Plan plan;
    plan.threadId = threadId;
    plan.title = input["title"].GetString();
    plan.objective = input["objective"].GetString();
    plan.context = input["context"].GetString();
    plan.strategy = input["strategy"].GetString();
    if (input.HasMember("notes")) {
      plan.notes = input["notes"].GetString();
    }

    const bool setActive =
        !input.HasMember("set_active") || input["set_active"].GetBool();
    if (input.HasMember("status")) {
      plan.status = worktools::parsePlanStatus(input["status"].GetString());
    } else {
      plan.status = setActive ? shared::PlanStatus::Active
                              : shared::PlanStatus::Draft;
    }

    const std::string planId = tm.createPlan(plan);
    const shared::Plan persistedPlan = tm.getPlan(threadId, planId);
    if (setActive) {
      auto metadata = tm.getMetadata(threadId);
      const bool activePlanChanged = metadata.activePlanId != planId;
      metadata.activePlanId = planId;
      tm.updateMetadata(threadId, metadata);
      if (activePlanChanged) {
        worktools::emitWorkEvent(
            shared::PlanCreated{threadId, persistedPlan});
        worktools::emitWorkEvent(
            shared::PlanActivated{threadId, persistedPlan.id, persistedPlan});
      } else {
        worktools::emitWorkEvent(shared::PlanCreated{threadId, persistedPlan});
      }
    } else {
      worktools::emitWorkEvent(shared::PlanCreated{threadId, persistedPlan});
    }
    rapidjson::Document doc;
    doc.SetObject();
    auto &alloc = doc.GetAllocator();
    doc.AddMember("plan_id", rapidjson::Value(planId.c_str(), alloc), alloc);
    std::string status = worktools::planStatusToString(persistedPlan.status);
    doc.AddMember("status", rapidjson::Value(status.c_str(), alloc), alloc);
    doc.AddMember("active", setActive, alloc);
    return shared::ToolResult::ok(doc);
  } catch (const std::exception &e) {
    return shared::ToolResult::fail(e.what());
  }
}

} // namespace firmius::core

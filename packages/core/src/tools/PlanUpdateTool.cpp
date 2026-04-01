#include "tools/PlanUpdateTool.hpp"
#include "tools/WorkToolCommon.hpp"
#include "utils/StringUtil.hpp"

namespace firmius::core {

namespace {

bool shouldApplyOptionalField(const rapidjson::Value &input, const char *field) {
  if (!input.HasMember(field)) {
    return false;
  }

  const auto &value = input[field];
  if (value.IsNull()) {
    return false;
  }
  if (value.IsString()) {
    return !firmius::shared::StringUtil::trim(
                std::string_view(value.GetString()))
                .empty();
  }
  return true;
}

} // namespace

shared::ToolMetadata PlanUpdateTool::getMetadata() const {
  return {"plan_update", "Update plan fields in the current thread",
          shared::ToolScope::PlanWrite};
}

std::shared_ptr<shared::JSONSchema> PlanUpdateTool::getSchema() const {
  return zObject({
             {"plan_id", zString()},
             {"title", zString()->setOptional()},
             {"objective", zString()->setOptional()},
             {"context", zString()->setOptional()},
             {"strategy", zString()->setOptional()},
             {"notes", zString()->setOptional()},
             {"status",
              zEnum({"Draft", "Active", "Paused", "Done", "Abandoned"})
                  ->setOptional()},
         })
      ->required({"plan_id"});
}

shared::ToolResult PlanUpdateTool::execute(const rapidjson::Value &input,
                                           shared::ToolContext &ctx) {
  try {
    worktools::requirePlanWriteAccess(ctx);
    const std::string threadId = worktools::requireCurrentThreadId(ctx);
    auto tm = worktools::makeThreadManager();
    shared::Plan plan =
        worktools::loadPlan(tm, threadId, input["plan_id"].GetString());

    if (shouldApplyOptionalField(input, "title")) {
      plan.title = input["title"].GetString();
    }
    if (shouldApplyOptionalField(input, "objective")) {
      plan.objective = input["objective"].GetString();
    }
    if (shouldApplyOptionalField(input, "context")) {
      plan.context = input["context"].GetString();
    }
    if (shouldApplyOptionalField(input, "strategy")) {
      plan.strategy = input["strategy"].GetString();
    }
    if (shouldApplyOptionalField(input, "notes")) {
      plan.notes = input["notes"].GetString();
    }
    if (shouldApplyOptionalField(input, "status")) {
      plan.status = worktools::parsePlanStatus(input["status"].GetString());
    }

    tm.updatePlan(threadId, plan);
    const shared::Plan updated = tm.getPlan(threadId, plan.id);
    worktools::emitWorkEvent(shared::PlanUpdated{threadId, updated});

    rapidjson::Document doc;
    doc.SetObject();
    auto &alloc = doc.GetAllocator();
    doc.AddMember("plan_id", rapidjson::Value(updated.id.c_str(), alloc), alloc);
    std::string status = worktools::planStatusToString(updated.status);
    doc.AddMember("status", rapidjson::Value(status.c_str(), alloc), alloc);
    return shared::ToolResult::ok(doc);
  } catch (const std::exception &e) {
    return shared::ToolResult::fail(e.what());
  }
}

} // namespace firmius::core

#include "tools/FleetTool.hpp"

#include "tools/FleetLockRespondTool.hpp"
#include "tools/FleetLockTool.hpp"
#include "tools/FleetStatusTool.hpp"

#include <rapidjson/document.h>

namespace firmius::core {

namespace {

std::string getActionString(const rapidjson::Value &input) {
  if (!input.IsObject() || !input.HasMember("action") ||
      !input["action"].IsString()) {
    return "";
  }
  return input["action"].GetString();
}

rapidjson::Document forwardedArgsWithoutAction(const rapidjson::Value &input) {
  rapidjson::Document forwarded;
  forwarded.SetObject();
  auto &alloc = forwarded.GetAllocator();
  if (!input.IsObject()) {
    return forwarded;
  }

  for (auto it = input.MemberBegin(); it != input.MemberEnd(); ++it) {
    if (std::string_view(it->name.GetString()) == "action") {
      continue;
    }
    rapidjson::Value key(it->name.GetString(), alloc);
    rapidjson::Value value;
    value.CopyFrom(it->value, alloc);
    forwarded.AddMember(key.Move(), value.Move(), alloc);
  }

  return forwarded;
}

template <typename Tool>
shared::ToolResult forwardTool(Tool &tool, const rapidjson::Value &input,
                               shared::ToolContext &ctx) {
  const rapidjson::Document forwarded = forwardedArgsWithoutAction(input);
  auto validation = tool.getSchema()->validate(forwarded);
  if (!validation.success) {
    return shared::ToolResult::fail(validation.violationToPretty());
  }
  return static_cast<shared::ITool &>(tool).execute(forwarded, ctx);
}

} // namespace

shared::ToolMetadata FleetTool::getMetadata() const {
  return {"Fleet",
          "Fleet coordination operations. Use action Lock, Respond, or Status.",
          shared::ToolScope::Delegation};
}

std::shared_ptr<shared::JSONSchema> FleetTool::getSchema() const {
  return shared::zObject({
      {"action", shared::zEnum({"Lock", "Respond", "Status"})
                     ->describe(
                         "Fleet coordination action to execute.\n\n"
                         "- Lock: acquire/release/check a fleet lock flow\n"
                         "- Respond: answer a pending lock request\n"
                         "- Status: inspect lock/fleet state")},
      {"mode", shared::zString()->setOptional()->describe(
          "Lock mode / operation detail, mainly for action=Lock (for example acquire, release, request, wait, check depending on downstream lock handler).")},
      {"request_id", shared::zString()->setOptional()->describe(
          "Pending fleet lock request id for action=Respond.")},
      {"lock_id", shared::zString()->setOptional()->describe(
          "Lock id or lock handle for lock-oriented operations.")},
  });
}

shared::ToolResult FleetTool::execute(const rapidjson::Value &input,
                                      shared::ToolContext &ctx) {
  const std::string action = getActionString(input);
  if (action == "Lock") {
    FleetLockTool tool;
    return forwardTool(tool, input, ctx);
  }
  if (action == "Respond") {
    FleetLockRespondTool tool;
    return forwardTool(tool, input, ctx);
  }
  if (action == "Status") {
    FleetStatusTool tool;
    return forwardTool(tool, input, ctx);
  }
  return shared::ToolResult::fail("Fleet.action must be Lock, Respond, or Status");
}

} // namespace firmius::core

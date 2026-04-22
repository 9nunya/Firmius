#include "tools/WebTool.hpp"

#include "tools/WebFetchTool.hpp"
#include "tools/WebSearchTool.hpp"

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

shared::ToolMetadata WebTool::getMetadata() const {
  return {"Web",
          "Web operations. Use action Fetch or Search.",
          shared::ToolScope::Web};
}

std::shared_ptr<shared::JSONSchema> WebTool::getSchema() const {
  return shared::zObject({
      {"action", shared::zEnum({"Fetch", "Search"})
                     ->describe("Web operation to execute")},
      {"url", shared::zString()->setOptional()},
      {"query", shared::zString()->setOptional()},
      {"urls", shared::zArray(shared::zString())->setOptional()},
  });
}

shared::ToolResult WebTool::execute(const rapidjson::Value &input,
                                    shared::ToolContext &ctx) {
  const std::string action = getActionString(input);
  if (action == "Fetch") {
    WebFetchTool tool;
    return forwardTool(tool, input, ctx);
  }
  if (action == "Search") {
    WebSearchTool tool;
    return forwardTool(tool, input, ctx);
  }
  return shared::ToolResult::fail("Web.action must be Fetch or Search");
}

} // namespace firmius::core

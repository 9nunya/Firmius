#include "tools/WorkTool.hpp"

#include <rapidjson/document.h>

namespace firmius::core {

shared::ToolMetadata WorkTool::getMetadata() const {
  return {"Work",
          R"(Removed. The old plan/chunk orchestration runtime no longer exists.
The todo system remains available through the Todo tool.
)",
          shared::ToolScope::Semantic};
}

std::shared_ptr<shared::JSONSchema> WorkTool::getSchema() const {
  return shared::zObject({
      {"action", shared::zString()->setOptional()},
  });
}

shared::ToolResult WorkTool::execute(const rapidjson::Value &, shared::ToolContext &) {
  return shared::ToolResult::fail(
      "Work tool has been removed. Use Todo for execution tracking.");
}

} // namespace firmius::core

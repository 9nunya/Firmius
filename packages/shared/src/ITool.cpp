#include "ITool.hpp"

#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

namespace firmius::shared {

ToolResult ToolResult::ok(const rapidjson::Document &doc,
                          const std::string &processId,
                          const std::string &subagentId) {
  rapidjson::StringBuffer sb;
  rapidjson::Writer<rapidjson::StringBuffer> writer(sb);
  doc.Accept(writer);
  return ok(std::string(sb.GetString()), processId, subagentId);
}

} // namespace firmius::shared

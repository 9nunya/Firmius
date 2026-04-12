#ifndef FIRMIUS_CORE_MCP_READ_RESOURCE_TOOL_HPP
#define FIRMIUS_CORE_MCP_READ_RESOURCE_TOOL_HPP

#include "ITool.hpp"

#include <string>

namespace firmius::core {

struct McpReadResourceInput {
  std::string server_name;
  std::string uri;
  int timeout_ms = 30000;
};

class McpReadResourceTool : public shared::TypedTool<McpReadResourceInput> {
public:
  shared::ToolMetadata getMetadata() const override;
  std::shared_ptr<shared::JSONSchema> getSchema() const override;

  START_MAPPING(McpReadResourceInput)
  MAP_STRING(server_name, "server_name")
  MAP_STRING(uri, "uri")
  MAP_INT(timeout_ms, "timeout_ms")
  END_MAPPING

  shared::ToolResult execute(const McpReadResourceInput &input,
                             shared::ToolContext &ctx) override;
};

} // namespace firmius::core

#endif

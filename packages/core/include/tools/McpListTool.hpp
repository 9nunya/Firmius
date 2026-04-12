#ifndef FIRMIUS_CORE_MCP_LIST_TOOL_HPP
#define FIRMIUS_CORE_MCP_LIST_TOOL_HPP

#include "ITool.hpp"

#include <string>

namespace firmius::core {

struct McpListInput {
  std::string server_name;
  int timeout_ms = 30000;
};

class McpListTool : public shared::TypedTool<McpListInput> {
public:
  shared::ToolMetadata getMetadata() const override;
  std::shared_ptr<shared::JSONSchema> getSchema() const override;

  START_MAPPING(McpListInput)
  MAP_STRING(server_name, "server_name")
  MAP_INT(timeout_ms, "timeout_ms")
  END_MAPPING

  shared::ToolResult execute(const McpListInput &input,
                             shared::ToolContext &ctx) override;
};

} // namespace firmius::core

#endif

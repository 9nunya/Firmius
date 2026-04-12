#ifndef FIRMIUS_CORE_MCP_SEARCH_TOOL_HPP
#define FIRMIUS_CORE_MCP_SEARCH_TOOL_HPP

#include "ITool.hpp"

#include <string>

namespace firmius::core {

struct McpSearchInput {
  std::string query;
  std::string server_name;
  int timeout_ms = 30000;
};

class McpSearchTool : public shared::TypedTool<McpSearchInput> {
public:
  shared::ToolMetadata getMetadata() const override;
  std::shared_ptr<shared::JSONSchema> getSchema() const override;

  START_MAPPING(McpSearchInput)
  MAP_STRING(query, "query")
  MAP_STRING(server_name, "server_name")
  MAP_INT(timeout_ms, "timeout_ms")
  END_MAPPING

  shared::ToolResult execute(const McpSearchInput &input,
                             shared::ToolContext &ctx) override;
};

} // namespace firmius::core

#endif

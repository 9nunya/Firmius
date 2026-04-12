#ifndef FIRMIUS_CORE_MCP_CALL_TOOL_HPP
#define FIRMIUS_CORE_MCP_CALL_TOOL_HPP

#include "ITool.hpp"

#include <rapidjson/document.h>
#include <string>

namespace firmius::core {

struct McpCallInput {
  std::string server_name;
  std::string tool_name;
  rapidjson::Document arguments;
  int timeout_ms = 30000;
};

class McpCallTool : public shared::TypedTool<McpCallInput> {
public:
  shared::ToolMetadata getMetadata() const override;
  std::shared_ptr<shared::JSONSchema> getSchema() const override;
  McpCallInput transform(const rapidjson::Value &json) override;
  shared::ToolResult execute(const McpCallInput &input,
                             shared::ToolContext &ctx) override;
};

} // namespace firmius::core

#endif
#ifndef FIRMIUS_CORE_MCP_LOAD_TOOL_HPP
#define FIRMIUS_CORE_MCP_LOAD_TOOL_HPP

#include "ITool.hpp"

#include <string>
#include <vector>

namespace firmius::core {

struct McpLoadInput {
  std::string server_name;
  std::vector<std::string> tools;
  std::vector<std::string> resources;
  std::vector<std::string> prompts;
  int timeout_ms = 30000;
};

class McpLoadTool : public shared::TypedTool<McpLoadInput> {
public:
  shared::ToolMetadata getMetadata() const override;
  std::shared_ptr<shared::JSONSchema> getSchema() const override;
  McpLoadInput transform(const rapidjson::Value &json) override;
  shared::ToolResult execute(const McpLoadInput &input,
                             shared::ToolContext &ctx) override;
};

} // namespace firmius::core

#endif

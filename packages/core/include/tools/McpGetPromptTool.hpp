#ifndef FIRMIUS_CORE_MCP_GET_PROMPT_TOOL_HPP
#define FIRMIUS_CORE_MCP_GET_PROMPT_TOOL_HPP

#include "ITool.hpp"

#include <rapidjson/document.h>
#include <string>

namespace firmius::core {

struct McpGetPromptInput {
  std::string server_name;
  std::string prompt_name;
  rapidjson::Document arguments;
  int timeout_ms = 30000;
};

class McpGetPromptTool : public shared::TypedTool<McpGetPromptInput> {
public:
  shared::ToolMetadata getMetadata() const override;
  std::shared_ptr<shared::JSONSchema> getSchema() const override;
  McpGetPromptInput transform(const rapidjson::Value &json) override;
  shared::ToolResult execute(const McpGetPromptInput &input,
                             shared::ToolContext &ctx) override;
};

} // namespace firmius::core

#endif

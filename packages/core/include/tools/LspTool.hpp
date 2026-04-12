#ifndef FIRMIUS_CORE_LSP_TOOL_HPP
#define FIRMIUS_CORE_LSP_TOOL_HPP

#include "ITool.hpp"
#include <string>

namespace firmius::core {

struct LspToolInput {
  std::string operation;
  std::string path;
  std::string project_root;
  std::string query;
  int line = 1;
  int character = 1;
  bool include_declaration = true;
  int max_results = 50;
  int timeout_ms = 30000;
};

class LspTool : public shared::TypedTool<LspToolInput> {
public:
  shared::ToolMetadata getMetadata() const override;
  std::shared_ptr<shared::JSONSchema> getSchema() const override;
  LspToolInput transform(const rapidjson::Value &json) override;
  shared::ToolResult execute(const LspToolInput &input,
                             shared::ToolContext &ctx) override;
};

}

#endif

#ifndef FIRMIUS_CORE_GREPTOOL_HPP
#define FIRMIUS_CORE_GREPTOOL_HPP

#include "ITool.hpp"

namespace firmius::core {

class GrepTool : public shared::ITool {
public:
  shared::ToolMetadata getMetadata() const override;
  std::shared_ptr<shared::JSONSchema> getSchema() const override;
  shared::ToolResult execute(const rapidjson::Value &input,
                             shared::ToolContext &ctx) override;
};

} // namespace firmius::core

#endif

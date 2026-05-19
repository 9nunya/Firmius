#ifndef FIRMIUS_CORE_SKILLLOADTOOL_HPP
#define FIRMIUS_CORE_SKILLLOADTOOL_HPP

#include "ITool.hpp"
#include <string>

namespace firmius::core {

struct SkillLoadInput {
  std::string what;
};

class SkillLoadTool : public shared::TypedTool<SkillLoadInput> {
public:
  shared::ToolMetadata getMetadata() const override;
  std::shared_ptr<shared::JSONSchema> getSchema() const override;

  START_MAPPING(SkillLoadInput)
  MAP_STRING(what, "what")
  END_MAPPING

  shared::ToolResult execute(const SkillLoadInput &input,
                             shared::ToolContext &ctx) override;
};

} // namespace firmius::core

#endif
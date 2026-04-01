#ifndef FIRMIUS_CORE_FLEET_STATUS_TOOL_HPP
#define FIRMIUS_CORE_FLEET_STATUS_TOOL_HPP

#include "ITool.hpp"
#include <optional>
#include <string>

namespace firmius::core {

struct FleetStatusInput {
  std::optional<std::string> root_agent_id;
  bool include_closed = false;
};

class FleetStatusTool : public shared::TypedTool<FleetStatusInput> {
public:
  shared::ToolMetadata getMetadata() const override;
  std::shared_ptr<shared::JSONSchema> getSchema() const override;

  START_MAPPING(FleetStatusInput)
    if (json.HasMember("root_agent_id") && json["root_agent_id"].IsString()) {
      input.root_agent_id = json["root_agent_id"].GetString();
    }
    if (json.HasMember("include_closed") && json["include_closed"].IsBool()) {
      input.include_closed = json["include_closed"].GetBool();
    }
  END_MAPPING

  shared::ToolResult execute(const FleetStatusInput &input,
                             shared::ToolContext &ctx) override;
};

} // namespace firmius::core

#endif

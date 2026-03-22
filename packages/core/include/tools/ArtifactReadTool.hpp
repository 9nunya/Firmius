#ifndef FIRMIUS_CORE_ARTIFACT_READ_TOOL_HPP
#define FIRMIUS_CORE_ARTIFACT_READ_TOOL_HPP

#include "ITool.hpp"
#include <optional>
#include <string>

namespace firmius::core {

struct ArtifactReadInput {
  std::optional<std::string> reference;
  std::optional<std::string> name;
  std::optional<std::string> owner_friendly_name;
  std::optional<std::string> owner_agent_id;
};

class ArtifactReadTool : public shared::TypedTool<ArtifactReadInput> {
public:
  shared::ToolMetadata getMetadata() const override;
  std::shared_ptr<shared::JSONSchema> getSchema() const override;

  START_MAPPING(ArtifactReadInput)
  if (json.HasMember("reference") && json["reference"].IsString()) {
    input.reference = json["reference"].GetString();
  }
  if (json.HasMember("name") && json["name"].IsString()) {
    input.name = json["name"].GetString();
  }
  if (json.HasMember("owner_friendly_name") &&
      json["owner_friendly_name"].IsString()) {
    input.owner_friendly_name = json["owner_friendly_name"].GetString();
  }
  if (json.HasMember("owner_agent_id") && json["owner_agent_id"].IsString()) {
    input.owner_agent_id = json["owner_agent_id"].GetString();
  }
  END_MAPPING

  shared::ToolResult execute(const ArtifactReadInput &input,
                             shared::ToolContext &ctx) override;
};

} // namespace firmius::core

#endif

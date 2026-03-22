#ifndef FIRMIUS_CORE_ARTIFACT_WRITE_TOOL_HPP
#define FIRMIUS_CORE_ARTIFACT_WRITE_TOOL_HPP

#include "ITool.hpp"
#include <optional>
#include <string>

namespace firmius::core {

struct ArtifactWriteInput {
  std::string name;
  std::string content;
  std::optional<std::string> kind;
  std::optional<std::string> description;
};

class ArtifactWriteTool : public shared::TypedTool<ArtifactWriteInput> {
public:
  shared::ToolMetadata getMetadata() const override;
  std::shared_ptr<shared::JSONSchema> getSchema() const override;

  START_MAPPING(ArtifactWriteInput)
  MAP_STRING(name, "name")
  MAP_STRING(content, "content")
  if (json.HasMember("kind") && json["kind"].IsString()) {
    input.kind = json["kind"].GetString();
  }
  if (json.HasMember("description") && json["description"].IsString()) {
    input.description = json["description"].GetString();
  }
  END_MAPPING

  shared::ToolResult execute(const ArtifactWriteInput &input,
                             shared::ToolContext &ctx) override;
};

} // namespace firmius::core

#endif

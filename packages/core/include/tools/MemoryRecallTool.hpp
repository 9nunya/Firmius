#ifndef FIRMIUS_CORE_MEMORY_RECALL_TOOL_HPP
#define FIRMIUS_CORE_MEMORY_RECALL_TOOL_HPP

#include "ITool.hpp"

#include <optional>
#include <string>

namespace firmius::core {

struct MemoryRecallInput {
  std::optional<std::string> start_turn_id;
  std::optional<std::string> end_turn_id;
  std::optional<std::string> cursor_turn_id;
  std::optional<int> page;
  std::optional<int> page_size;
  std::optional<bool> include_system;
};

class MemoryRecallTool : public shared::TypedTool<MemoryRecallInput> {
public:
  shared::ToolMetadata getMetadata() const override;
  std::shared_ptr<shared::JSONSchema> getSchema() const override;

  START_MAPPING(MemoryRecallInput)
  if (json.HasMember("start_turn_id") && json["start_turn_id"].IsString()) {
    input.start_turn_id = json["start_turn_id"].GetString();
  }
  if (json.HasMember("end_turn_id") && json["end_turn_id"].IsString()) {
    input.end_turn_id = json["end_turn_id"].GetString();
  }
  if (json.HasMember("cursor_turn_id") && json["cursor_turn_id"].IsString()) {
    input.cursor_turn_id = json["cursor_turn_id"].GetString();
  }
  if (json.HasMember("page") && json["page"].IsInt()) {
    input.page = json["page"].GetInt();
  }
  if (json.HasMember("page_size") && json["page_size"].IsInt()) {
    input.page_size = json["page_size"].GetInt();
  }
  if (json.HasMember("include_system") && json["include_system"].IsBool()) {
    input.include_system = json["include_system"].GetBool();
  }
  END_MAPPING

  shared::ToolResult execute(const MemoryRecallInput &input,
                             shared::ToolContext &ctx) override;
};

} // namespace firmius::core

#endif

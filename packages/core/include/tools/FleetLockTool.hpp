#ifndef FIRMIUS_CORE_FLEET_LOCK_TOOL_HPP
#define FIRMIUS_CORE_FLEET_LOCK_TOOL_HPP

#include "ITool.hpp"
#include <optional>
#include <string>
#include <vector>

namespace firmius::core {

/**
 * @brief Consolidated lock tool for acquire/release/request operations.
 * 
 * Modes:
 * - acquire: Take a lock on files (default)
 * - release: Release a lock you own
 * - request: Ask another agent to lock files and notify when done
 * - wait: Wait for a specific lock to be released
 * - check: Check lock status without blocking
 */
struct FleetLockInput {
  std::string mode = "acquire";
  std::string lock_id;
  std::string reason;
  std::vector<std::string> paths;
  std::optional<std::string> target_agent_id;  // For request mode
  std::optional<int> timeout_ms;
};

class FleetLockTool : public shared::TypedTool<FleetLockInput> {
public:
  shared::ToolMetadata getMetadata() const override;
  std::shared_ptr<shared::JSONSchema> getSchema() const override;

  START_MAPPING(FleetLockInput)
    if (json.HasMember("mode") && json["mode"].IsString()) {
      input.mode = json["mode"].GetString();
    }
    if (json.HasMember("lock_id") && json["lock_id"].IsString()) {
      input.lock_id = json["lock_id"].GetString();
    }
    if (json.HasMember("reason") && json["reason"].IsString()) {
      input.reason = json["reason"].GetString();
    }
    if (json.HasMember("paths") && json["paths"].IsArray()) {
      for (const auto &entry : json["paths"].GetArray()) {
        if (entry.IsString()) {
          input.paths.push_back(entry.GetString());
        }
      }
    }
    if (json.HasMember("target_agent_id") && json["target_agent_id"].IsString()) {
      input.target_agent_id = json["target_agent_id"].GetString();
    }
    if (json.HasMember("timeout_ms") && json["timeout_ms"].IsInt()) {
      input.timeout_ms = json["timeout_ms"].GetInt();
    }
  END_MAPPING

  shared::ToolResult execute(const FleetLockInput &input,
                             shared::ToolContext &ctx) override;
};

} // namespace firmius::core

#endif

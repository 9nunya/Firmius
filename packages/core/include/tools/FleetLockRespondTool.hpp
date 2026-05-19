#ifndef FIRMIUS_CORE_FLEETLOCKRESPONDTOOL_HPP
#define FIRMIUS_CORE_FLEETLOCKRESPONDTOOL_HPP

#include "ITool.hpp"
#include <optional>
#include <string>

namespace firmius::core {

/**
 * @brief Respond to a lock request from another agent.
 * 
 * Worker B uses this to accept or deny a lock request from Worker A.
 * - accept: Will complete work then release lock, requester waits
 * - deny: Already done or can't do, requester unblocks immediately
 */
struct FleetLockRespondInput {
  std::string request_id;
  bool accept;  // true = accept, false = deny
  std::optional<std::string> deny_reason;
  std::optional<int> estimated_ms;  // ETA when accepting
};

class FleetLockRespondTool : public shared::TypedTool<FleetLockRespondInput> {
public:
  shared::ToolMetadata getMetadata() const override;
  std::shared_ptr<shared::JSONSchema> getSchema() const override;

  START_MAPPING(FleetLockRespondInput)
    MAP_STRING(request_id, "request_id")
    MAP_BOOL(accept, "accept")
    if (json.HasMember("deny_reason") && json["deny_reason"].IsString()) {
      input.deny_reason = json["deny_reason"].GetString();
    }
    if (json.HasMember("estimated_ms") && json["estimated_ms"].IsInt()) {
      input.estimated_ms = json["estimated_ms"].GetInt();
    }
  END_MAPPING

  shared::ToolResult execute(const FleetLockRespondInput &input,
                             shared::ToolContext &ctx) override;
};

} // namespace firmius::core

#endif

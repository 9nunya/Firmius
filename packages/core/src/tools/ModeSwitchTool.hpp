#ifndef FIRMIUS_CORE_MODESWITCHTOOL_HPP
#define FIRMIUS_CORE_MODESWITCHTOOL_HPP

#include "ITool.hpp"

#include <optional>
#include <string>

namespace firmius::core {

/**
 * @brief Argument struct for ModeSwitchTool.
 *
 * `name` accepts either a bare sub-mode name (resolved against the active
 * persona's sub-mode set first, then system modes as fallback) or a fully
 * qualified `persona:submode` form. Empty string clears the active mode.
 */
struct ModeSwitchInput {
  std::string name;
  std::optional<std::string> reason; ///< why the agent is switching (logged)
};

/**
 * @brief Switches the active mode of the calling agent.
 *
 * Validation:
 *   - The mode must be registered in `ModeRegistry`.
 *   - If persona-scoped, the scope must match the calling agent's persona.
 *   - If the current mode declares `allowed_transitions_to`, the requested
 *     mode must be in that list (qualified-name match).
 *
 * Side-effects:
 *   - Mutates `AgentState::activeMode` (visible to next prompt composition).
 *   - Will fire `mode_exited` + `mode_entered` events when the hook
 *     dispatcher is wired to listen on this tool path.
 */
class ModeSwitchTool : public shared::TypedTool<ModeSwitchInput> {
public:
  shared::ToolMetadata getMetadata() const override;
  std::shared_ptr<shared::JSONSchema> getSchema() const override;

  START_MAPPING(ModeSwitchInput)
  if (json.HasMember("name") && json["name"].IsString()) {
    input.name = json["name"].GetString();
  }
  if (json.HasMember("reason") && json["reason"].IsString()) {
    input.reason = json["reason"].GetString();
  }
  END_MAPPING

  shared::ToolResult execute(const ModeSwitchInput &input,
                             shared::ToolContext &ctx) override;
};

} // namespace firmius::core

#endif

#ifndef FIRMIUS_CORE_PINTOOL_HPP
#define FIRMIUS_CORE_PINTOOL_HPP

#include "ITool.hpp"

namespace firmius::core {

/**
 * @brief Working-memory pin tool: agent-driven anchoring.
 *
 * The agent calls this tool when it identifies a fact, decision, or
 * constraint that should survive aggressive eviction during long
 * sessions. The pin is stored on AgentState and consulted by the
 * working-memory PinPolicy on every request.
 *
 * Two action modes:
 *   - `add` (default): pin a free-text anchor and/or a turn id.
 *   - `remove`: unpin a previously-added anchor or turn id.
 */
class PinTool : public shared::ITool {
public:
  shared::ToolMetadata getMetadata() const override;
  std::shared_ptr<shared::JSONSchema> getSchema() const override;
  shared::ToolResult execute(const rapidjson::Value &input,
                             shared::ToolContext &ctx) override;
};

} // namespace firmius::core

#endif

#ifndef FIRMIUS_CORE_SUBAGENT_WAIT_TOOL_HPP
#define FIRMIUS_CORE_SUBAGENT_WAIT_TOOL_HPP

#include "ITool.hpp"
#include <string>

namespace firmius::core {

struct SubagentWaitInput {
    std::string agent_id;
};

class SubagentWaitTool : public shared::TypedTool<SubagentWaitInput> {
public:
    shared::ToolMetadata getMetadata() const override;
    std::shared_ptr<shared::JSONSchema> getSchema() const override;

    START_MAPPING(SubagentWaitInput)
        MAP_STRING(agent_id, "agent_id")
    END_MAPPING

    shared::ToolResult execute(const SubagentWaitInput& input, shared::ToolContext& ctx) override;
};

}

#endif

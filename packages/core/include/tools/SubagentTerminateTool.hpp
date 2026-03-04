#ifndef FIRMIUS_CORE_SUBAGENT_TERMINATE_TOOL_HPP
#define FIRMIUS_CORE_SUBAGENT_TERMINATE_TOOL_HPP

#include "ITool.hpp"
#include <string>

namespace firmius::core {

struct SubagentTerminateInput {
    std::string agent_id;
};

class SubagentTerminateTool : public shared::TypedTool<SubagentTerminateInput> {
public:
    shared::ToolMetadata getMetadata() const override;
    std::shared_ptr<shared::JSONSchema> getSchema() const override;

    START_MAPPING(SubagentTerminateInput)
        MAP_STRING(agent_id, "agent_id")
    END_MAPPING

    shared::ToolResult execute(const SubagentTerminateInput& input, shared::ToolContext& ctx) override;
};

}

#endif

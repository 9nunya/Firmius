#ifndef FIRMIUS_CORE_SUBAGENT_TOOL_HPP
#define FIRMIUS_CORE_SUBAGENT_TOOL_HPP

#include "ITool.hpp"
#include <string>

namespace firmius::core {

struct SubagentInput {
    std::string persona;
    std::string task;
    bool async = false;
};

class SubagentTool : public shared::TypedTool<SubagentInput> {
public:
    shared::ToolMetadata getMetadata() const override;
    std::shared_ptr<shared::JSONSchema> getSchema() const override;

    START_MAPPING(SubagentInput)
        MAP_STRING(persona, "persona")
        MAP_STRING(task, "task")
        MAP_BOOL(async, "async")
    END_MAPPING

    shared::ToolResult execute(const SubagentInput& input, shared::ToolContext& ctx) override;
};

}

#endif

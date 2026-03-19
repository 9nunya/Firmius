#ifndef FIRMIUS_CORE_SUBAGENT_TOOL_HPP
#define FIRMIUS_CORE_SUBAGENT_TOOL_HPP

#include "ITool.hpp"
#include <string>
#include <optional>

namespace firmius::core {

struct SubagentInput {
    std::string persona;
    std::string task;
    bool async = false;
    std::optional<std::string> agent_id;
    std::optional<std::string> plan_id;
    std::optional<std::string> chunk_id;
    std::optional<std::string> category;
    std::string name;   ///< Machine-friendly slug (e.g., "auth-finder")
    std::string title;  ///< Human-readable display name (e.g., "Find auth patterns")
};

class SubagentTool : public shared::TypedTool<SubagentInput> {
public:
    shared::ToolMetadata getMetadata() const override;
    std::shared_ptr<shared::JSONSchema> getSchema() const override;

    START_MAPPING(SubagentInput)
        MAP_STRING(persona, "persona")
        MAP_STRING(task, "task")
        MAP_BOOL(async, "async")
        if (json.HasMember("agent_id") && json["agent_id"].IsString()) {
            input.agent_id = json["agent_id"].GetString();
        }
        if (json.HasMember("plan_id") && json["plan_id"].IsString()) {
            input.plan_id = json["plan_id"].GetString();
        }
        if (json.HasMember("chunk_id") && json["chunk_id"].IsString()) {
            input.chunk_id = json["chunk_id"].GetString();
        }
        if (json.HasMember("category") && json["category"].IsString()) {
            input.category = json["category"].GetString();
        }
        MAP_STRING(name, "name")
        MAP_STRING(title, "title")
    END_MAPPING

    shared::ToolResult execute(const SubagentInput& input, shared::ToolContext& ctx) override;
};

}

#endif

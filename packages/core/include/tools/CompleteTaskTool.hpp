#ifndef FIRMIUS_CORE_COMPLETE_TASK_TOOL_HPP
#define FIRMIUS_CORE_COMPLETE_TASK_TOOL_HPP

#include "ITool.hpp"
#include <string>

namespace firmius::core {
using namespace firmius::shared;

struct CompleteTaskInput {
    std::string summary;
};

class CompleteTaskTool : public shared::TypedTool<CompleteTaskInput> {
public:
    shared::ToolMetadata getMetadata() const override {
        return {"complete_task", "Signal that the task is fully completed. Provide a summary of your work.", ToolScope::Process};
    }

    std::shared_ptr<shared::JSONSchema> getSchema() const override {
        return zObject({
            {"summary", zString()->describe("A brief summary of what was accomplished")}
        })->required({"summary"});
    }
    
    START_MAPPING(CompleteTaskInput)
        MAP_STRING(summary, "summary")
    END_MAPPING

    shared::ToolResult execute(const CompleteTaskInput&, shared::ToolContext&) override {
        return shared::ToolResult::ok();
    }
};

}

#endif

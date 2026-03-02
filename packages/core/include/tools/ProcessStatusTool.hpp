#ifndef FIRMIUS_CORE_PROCESS_STATUS_TOOL_HPP
#define FIRMIUS_CORE_PROCESS_STATUS_TOOL_HPP

#include "ITool.hpp"
#include <string>

namespace firmius::core {
using namespace firmius::shared;

struct ProcessStatusInput {
    std::string process_id;
};

class ProcessStatusTool : public shared::TypedTool<ProcessStatusInput> {
public:
    shared::ToolMetadata getMetadata() const override {
        return {"process_status", "Returns the current status and output of a background process.", ToolScope::Process};
    }

    std::shared_ptr<shared::JSONSchema> getSchema() const override {
        return zObject({
            {"process_id", zString()->describe("The UUID of the process")}
        })->required({"process_id"});
    }

    START_MAPPING(ProcessStatusInput)
        MAP_STRING(process_id, "process_id")
    END_MAPPING

    shared::ToolResult execute(const ProcessStatusInput& input, shared::ToolContext& ctx) override;
};

}

#endif

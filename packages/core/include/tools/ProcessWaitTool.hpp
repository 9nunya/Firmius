#ifndef FIRMIUS_CORE_PROCESS_WAIT_TOOL_HPP
#define FIRMIUS_CORE_PROCESS_WAIT_TOOL_HPP

#include "ITool.hpp"
#include <string>

namespace firmius::core {
using namespace firmius::shared;

struct ProcessWaitInput {
    std::string process_id;
    std::string pattern;
    int timeout_ms = 30000;
};

class ProcessWaitTool : public shared::TypedTool<ProcessWaitInput> {
public:
    shared::ToolMetadata getMetadata() const override {
        return {"process_wait", "Waits for a process to complete or for a pattern to appear in the output.", ToolScope::Process};
    }

    std::shared_ptr<shared::JSONSchema> getSchema() const override {
        return zObject({
            {"process_id", zString()->describe("The UUID of the process")},
            {"pattern", zString()->describe("Optional pattern to wait for in output")->setOptional()},
            {"timeout_ms", zInteger()->describe("Timeout in milliseconds (default 30s)")->setOptional()}
        })->required({"process_id"});
    }

    START_MAPPING(ProcessWaitInput)
        MAP_STRING(process_id, "process_id")
        MAP_STRING(pattern, "pattern")
        MAP_INT(timeout_ms, "timeout_ms")
    END_MAPPING

    shared::ToolResult execute(const ProcessWaitInput& input, shared::ToolContext& ctx) override;
};

}

#endif

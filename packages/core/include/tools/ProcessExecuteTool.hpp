#ifndef FIRMIUS_CORE_PROCESS_EXECUTE_TOOL_HPP
#define FIRMIUS_CORE_PROCESS_EXECUTE_TOOL_HPP

#include "ITool.hpp"
#include <string>

namespace firmius::core {
using namespace firmius::shared;

struct ProcessExecuteInput {
    std::string command;
    std::string cwd;
};

class ProcessExecuteTool : public shared::TypedTool<ProcessExecuteInput> {
public:
    shared::ToolMetadata getMetadata() const override;
    std::shared_ptr<shared::JSONSchema> getSchema() const override;
    
    START_MAPPING(ProcessExecuteInput)
        MAP_STRING(command, "command")
        MAP_STRING(cwd, "cwd")
    END_MAPPING

    shared::ToolResult execute(const ProcessExecuteInput& input, shared::ToolContext& ctx) override;
};

}

#endif

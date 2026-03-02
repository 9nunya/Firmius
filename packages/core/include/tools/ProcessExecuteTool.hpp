#ifndef FIRMIUS_CORE_PROCESS_EXECUTE_TOOL_HPP
#define FIRMIUS_CORE_PROCESS_EXECUTE_TOOL_HPP

#include "ITool.hpp"
#include <string>

namespace firmius::core {

using namespace firmius::shared;

/**
 * @brief Input parameters for the process_execute tool.
 */
struct ProcessExecuteInput {
    std::string command; ///< Shell command to execute.
    std::string cwd;     ///< Optional working directory.
    int timeout_ms = 15000; ///< Timeout in milliseconds (default 15000).
};

/**
 * @brief Tool for executing shell commands on the host.
 */
class ProcessExecuteTool : public shared::TypedTool<ProcessExecuteInput> {
public:
    shared::ToolMetadata getMetadata() const override;
    std::shared_ptr<shared::JSONSchema> getSchema() const override;
    
    START_MAPPING(ProcessExecuteInput)
        MAP_STRING(command, "command")
        MAP_STRING(cwd, "cwd")
        MAP_INT(timeout_ms, "timeout_ms")
    END_MAPPING

    shared::ToolResult execute(const ProcessExecuteInput& input, shared::ToolContext& ctx) override;
};

}

#endif

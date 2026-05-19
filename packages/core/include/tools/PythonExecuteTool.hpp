#ifndef FIRMIUS_CORE_PYTHONEXECUTETOOL_HPP
#define FIRMIUS_CORE_PYTHONEXECUTETOOL_HPP

#include "ITool.hpp"
#include <string>

namespace firmius::core {

struct PythonExecuteInput {
    std::string code;
    std::string venv;
};

class PythonExecuteTool : public shared::TypedTool<PythonExecuteInput> {
public:
    shared::ToolMetadata getMetadata() const override;
    std::shared_ptr<shared::JSONSchema> getSchema() const override;

    START_MAPPING(PythonExecuteInput)
        MAP_STRING(code, "code")
        MAP_STRING(venv, "venv")
    END_MAPPING

    shared::ToolResult execute(const PythonExecuteInput& input, shared::ToolContext& ctx) override;
};

}

#endif

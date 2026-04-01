#ifndef FIRMIUS_CORE_PROCESS_INPUT_TOOL_HPP
#define FIRMIUS_CORE_PROCESS_INPUT_TOOL_HPP

#include "ITool.hpp"
#include <string>

namespace firmius::core {
using namespace firmius::shared;

struct ProcessInputInput {
    std::string process_id;
    std::string input;
};

class ProcessInputTool : public shared::TypedTool<ProcessInputInput> {
public:
    shared::ToolMetadata getMetadata() const override {
        return {"process_input", 
            "Sends input to a background process's stdin. "
            "Supports control tags: {Enter}, {Tab}, {Esc}, {Backspace}, {Delete}, "
            "{Up}, {Down}, {Left}, {Right}, {Home}, {End}, {PageUp}, {PageDown}, "
            "{F1}-{F12}, {Ctrl+X} (e.g., {Ctrl+C}), {Alt+X}. "
            "Use literal newlines for multi-line input (each line sent with 1s delay). "
            "Or use \\n escape sequences which are translated to newlines.",
            ToolScope::Process};
    }

    std::shared_ptr<shared::JSONSchema> getSchema() const override {
        return zObject({
            {"process_id", zString()->describe("The UUID of the process")},
            {"input", zString()->describe(
                "Text to send. Supports: literal newlines, \\n escapes, "
                "and control tags like {Enter}, {Ctrl+C}, {F1}, etc.")}
        })->required({"process_id", "input"});
    }

    START_MAPPING(ProcessInputInput)
        MAP_STRING(process_id, "process_id")
        MAP_STRING(input, "input")
    END_MAPPING

    shared::ToolResult execute(const ProcessInputInput& input, shared::ToolContext& ctx) override;
};

}

#endif

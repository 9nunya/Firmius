#ifndef FIRMIUS_CORE_GREP_TOOL_HPP
#define FIRMIUS_CORE_GREP_TOOL_HPP

#include "ITool.hpp"
#include <string>

namespace firmius::core {
using namespace firmius::shared;

struct GrepInput {
    std::string pattern;
    std::string path;
    int context_before = 0;
    int context_after = 0;
};

class GrepTool : public shared::TypedTool<GrepInput> {
public:
    shared::ToolMetadata getMetadata() const override {
        return {"grep", "Search for a regex pattern in file contents", ToolScope::FilesystemRead};
    }

    std::shared_ptr<shared::JSONSchema> getSchema() const override {
        return zObject({
            {"pattern", zString()->describe("The regex pattern to search for")},
            {"path", zString()->describe("The path to search in")},
            {"context_before", zInteger()->describe("Number of lines of context before each match")->setOptional()},
            {"context_after", zInteger()->describe("Number of lines of context after each match")->setOptional()}
        })->required({"pattern", "path"});
    }

    START_MAPPING(GrepInput)
        MAP_STRING(pattern, "pattern")
        MAP_STRING(path, "path")
        MAP_INT(context_before, "context_before")
        MAP_INT(context_after, "context_after")
    END_MAPPING

    shared::ToolResult execute(const GrepInput& input, shared::ToolContext& ctx) override;
};

}

#endif

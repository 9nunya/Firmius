#ifndef FIRMIUS_CORE_GLOB_TOOL_HPP
#define FIRMIUS_CORE_GLOB_TOOL_HPP

#include "ITool.hpp"
#include <string>

namespace firmius::core {
using namespace firmius::shared;

struct GlobInput {
    std::string pattern;
    std::string path;
};

class GlobTool : public shared::TypedTool<GlobInput> {
public:
    shared::ToolMetadata getMetadata() const override {
        return {"glob", "Find files matching a pattern", ToolScope::FilesystemRead};
    }

    std::shared_ptr<shared::JSONSchema> getSchema() const override {
        return zObject({
            {"pattern", zString()->describe("The glob pattern (e.g. *.cpp)")},
            {"path", zString()->describe("The root path to search in")}
        })->required({"pattern", "path"});
    }

    START_MAPPING(GlobInput)
        MAP_STRING(pattern, "pattern")
        MAP_STRING(path, "path")
    END_MAPPING

    shared::ToolResult execute(const GlobInput& input, shared::ToolContext& ctx) override;
};

}

#endif

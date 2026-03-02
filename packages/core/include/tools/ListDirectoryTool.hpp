#ifndef FIRMIUS_CORE_LIST_DIRECTORY_TOOL_HPP
#define FIRMIUS_CORE_LIST_DIRECTORY_TOOL_HPP

#include "ITool.hpp"
#include <string>

namespace firmius::core {
using namespace firmius::shared;

struct ListDirectoryInput {
    std::string path;
    bool include_hidden = false;
};

class ListDirectoryTool : public shared::TypedTool<ListDirectoryInput> {
public:
    shared::ToolMetadata getMetadata() const override {
        return {"list_directory", "List files and directories in a path", ToolScope::FilesystemRead};
    }

    std::shared_ptr<shared::JSONSchema> getSchema() const override {
        return zObject({
            {"path", zString()->describe("The path to list")},
            {"include_hidden", zBoolean()->describe("Whether to include hidden files (starting with .)")}
        })->required({"path"});
    }

    START_MAPPING(ListDirectoryInput)
        MAP_STRING(path, "path")
        MAP_BOOL(include_hidden, "include_hidden")
    END_MAPPING

    shared::ToolResult execute(const ListDirectoryInput& input, shared::ToolContext& ctx) override;
};

}

#endif

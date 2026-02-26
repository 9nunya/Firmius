#ifndef FIRMIUS_CORE_FILE_EDIT_TOOL_HPP
#define FIRMIUS_CORE_FILE_EDIT_TOOL_HPP

#include "ITool.hpp"
#include <string>

namespace firmius::core {
using namespace firmius::shared;

struct FileEditInput {
    std::string path;
    std::string content;
    std::string old_string;
    std::string new_string;
    bool replace_all = false;
};

class FileEditTool : public shared::TypedTool<FileEditInput> {
public:
    shared::ToolMetadata getMetadata() const override;
    std::shared_ptr<shared::JSONSchema> getSchema() const override;
    
    START_MAPPING(FileEditInput)
        MAP_STRING(path, "path")
        MAP_STRING(content, "content")
        MAP_STRING(old_string, "old_string")
        MAP_STRING(new_string, "new_string")
        MAP_BOOL(replace_all, "replace_all")
    END_MAPPING

    shared::ToolResult execute(const FileEditInput& input, shared::ToolContext& ctx) override;
};

}

#endif

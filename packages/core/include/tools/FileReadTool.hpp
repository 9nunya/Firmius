#ifndef FIRMIUS_CORE_FILE_READ_TOOL_HPP
#define FIRMIUS_CORE_FILE_READ_TOOL_HPP

#include "ITool.hpp"
#include <string>

namespace firmius::core {
using namespace firmius::shared;

struct FileReadInput {
    std::string path;
    int start_line = 1;
    int end_line = -1;
};

class FileReadTool : public shared::TypedTool<FileReadInput> {
public:
    shared::ToolMetadata getMetadata() const override;
    std::shared_ptr<shared::JSONSchema> getSchema() const override;
    
    START_MAPPING(FileReadInput)
        MAP_STRING(path, "path")
        MAP_INT(start_line, "start_line")
        MAP_INT(end_line, "end_line")
    END_MAPPING

    shared::ToolResult execute(const FileReadInput& input, shared::ToolContext& ctx) override;
};

}

#endif

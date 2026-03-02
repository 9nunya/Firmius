#ifndef FIRMIUS_CORE_FILE_READ_TOOL_HPP
#define FIRMIUS_CORE_FILE_READ_TOOL_HPP

#include "ITool.hpp"
#include <string>

namespace firmius::core {

using namespace firmius::shared;

/**
 * @brief Input parameters for the file_read tool.
 */
struct FileReadInput {
    std::string path;       ///< Path to the file.
    int start_line = 1;     ///< Optional start line (1-indexed).
    int end_line = -1;      ///< Optional end line (inclusive).
};

/**
 * @brief Tool for reading files from the host filesystem.
 */
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

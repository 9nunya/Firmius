#ifndef FIRMIUS_CORE_FILE_EDIT_TOOL_HPP
#define FIRMIUS_CORE_FILE_EDIT_TOOL_HPP

#include "ITool.hpp"
#include <string>

namespace firmius::core {

using namespace firmius::shared;

/**
 * @brief Input parameters for the file_edit tool.
 */
struct FileEditInput {
    std::string path;           ///< Path to the file.
    std::string content;        ///< New content (for overwrite mode).
    std::string old_string;     ///< Target string (for replace mode).
    std::string new_string;     ///< Replacement string (for replace mode).
    bool replace_all = false;   ///< Whether to replace all occurrences.
    float fuzzy_threshold = 1.0f; ///< Similarity threshold for fuzzy matching.
};

/**
 * @brief Tool for editing or overwriting files on the host filesystem.
 */
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
        MAP_FLOAT(fuzzy_threshold, "fuzzy_threshold")
    END_MAPPING

    shared::ToolResult execute(const FileEditInput& input, shared::ToolContext& ctx) override;
};

}

#endif

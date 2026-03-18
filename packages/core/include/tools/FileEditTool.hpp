#ifndef FIRMIUS_CORE_FILE_EDIT_TOOL_HPP
#define FIRMIUS_CORE_FILE_EDIT_TOOL_HPP

#include "ITool.hpp"
#include <string>
#include <vector>

namespace firmius::core {

using namespace firmius::shared;

struct FileEditOperationInput {
    std::string op;
    std::string start_anchor;
    std::string end_anchor;
    std::string anchor;
    std::vector<std::string> new_lines;
};

/**
 * @brief Input parameters for the file_edit tool.
 */
struct FileEditInput {
    std::string path;           ///< Path to the file.
    std::string content;        ///< Full content for whole-file overwrite mode.
    bool has_content = false;   ///< Whether content was provided.
    std::vector<FileEditOperationInput> edits; ///< Hashline-anchored edit operations.
    std::string old_string;     ///< Legacy substring replacement target.
    std::string new_string;     ///< Legacy substring replacement replacement text.
    bool has_old_string = false; ///< Whether old_string was provided.
    bool has_new_string = false; ///< Whether new_string was provided.
    bool replace_all = false;   ///< Legacy substring replacement flag.
    float fuzzy_threshold = 1.0f; ///< Legacy fuzzy replacement threshold.
};

/**
 * @brief Tool for editing or overwriting files on the host filesystem.
 */
class FileEditTool : public shared::TypedTool<FileEditInput> {
public:
    shared::ToolMetadata getMetadata() const override;
    std::shared_ptr<shared::JSONSchema> getSchema() const override;

    FileEditInput transform(const rapidjson::Value& json) override;

    shared::ToolResult execute(const FileEditInput& input, shared::ToolContext& ctx) override;
};

}

#endif

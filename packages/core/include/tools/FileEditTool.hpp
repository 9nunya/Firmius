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
    std::string old_string;
    std::string new_string;
    bool has_old_string = false;
    bool has_new_string = false;
    bool replace_all = false;
    int patch_line = 0; ///< Original line in patch text (for diagnostics).
};

struct FilePatchInput {
    std::string patch;
    bool validate_only = false;
};

struct FileWriteInput {
    std::string path;
    std::string content;
    bool validate_only = false;
};

struct FileReplaceInput {
    struct Replacement {
        std::string old_string;
        std::string new_string;
        bool replace_all = false;
    };

    std::string path;
    std::vector<Replacement> replacements;
    bool validate_only = false;
};

struct FileRangeInput {
    std::string path;
    std::vector<FileEditOperationInput> operations;
    bool validate_only = false;
};

class FileEditTool : public shared::TypedTool<FilePatchInput> {
public:
    shared::ToolMetadata getMetadata() const override;
    std::shared_ptr<shared::JSONSchema> getSchema() const override;
    FilePatchInput transform(const rapidjson::Value& json) override;
    shared::ToolResult execute(const FilePatchInput& input, shared::ToolContext& ctx) override;
};

class FileWriteTool : public shared::TypedTool<FileWriteInput> {
public:
    shared::ToolMetadata getMetadata() const override;
    std::shared_ptr<shared::JSONSchema> getSchema() const override;
    FileWriteInput transform(const rapidjson::Value& json) override;
    shared::ToolResult execute(const FileWriteInput& input, shared::ToolContext& ctx) override;
};

class FileReplaceTool : public shared::TypedTool<FileReplaceInput> {
public:
    shared::ToolMetadata getMetadata() const override;
    std::shared_ptr<shared::JSONSchema> getSchema() const override;
    FileReplaceInput transform(const rapidjson::Value& json) override;
    shared::ToolResult execute(const FileReplaceInput& input, shared::ToolContext& ctx) override;
};

class FileRangeTool : public shared::TypedTool<FileRangeInput> {
public:
    shared::ToolMetadata getMetadata() const override;
    std::shared_ptr<shared::JSONSchema> getSchema() const override;
    FileRangeInput transform(const rapidjson::Value& json) override;
    shared::ToolResult execute(const FileRangeInput& input, shared::ToolContext& ctx) override;
};

} // namespace firmius::core

#endif

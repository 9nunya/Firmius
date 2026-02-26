#include "tools/FileEditTool.hpp"
#include "agents/Agent.hpp"
#include <filesystem>
#include <iostream>

namespace firmius::core {
using namespace firmius::shared;

shared::ToolMetadata FileEditTool::getMetadata() const {
    return {"file_edit", "Edit or overwrite a file on the host filesystem", ToolScope::FilesystemWrite};
}

std::shared_ptr<shared::JSONSchema> FileEditTool::getSchema() const {
    return zObject({
        {"path", zString()->describe("Absolute or relative path to the file")},
        {"content", zString()->describe("Full content to write to the file")->setOptional()},
        {"old_string", zString()->describe("Exact substring to replace")->setOptional()},
        {"new_string", zString()->describe("New content to substitute")->setOptional()},
        {"replace_all", zBoolean()->describe("If true, replaces all occurrences of old_string")->setOptional()}
    })->required({"path"});
}

shared::ToolResult FileEditTool::execute(const FileEditInput& input, shared::ToolContext& ctx) {
    std::string absolutePath = ctx.agent.resolvePath(input.path);

    // Security check
    bool allowed = false;
    for (const auto& p : ctx.agent.getContext().permissions.allowedPaths) {
        if (absolutePath.starts_with(p)) {
            allowed = true;
            break;
        }
    }
    if (!allowed && !ctx.agent.getContext().permissions.allowOutsideCwd) {
        return shared::ToolResult::fail("Access denied: path outside allowed directories: " + absolutePath);
    }

    try {
        if (!input.old_string.empty() && !input.new_string.empty()) {
            // Replace mode
            auto data = ctx.host.readFile(absolutePath);
            std::string content(data.begin(), data.end());
            
            size_t pos = content.find(input.old_string);
            if (pos == std::string::npos) {
                return shared::ToolResult::fail("old_string not found in file");
            }
            
            if (input.replace_all) {
                while (pos != std::string::npos) {
                    content.replace(pos, input.old_string.length(), input.new_string);
                    pos = content.find(input.old_string, pos + input.new_string.length());
                }
            } else {
                content.replace(pos, input.old_string.length(), input.new_string);
            }

            ctx.host.writeFile(absolutePath, std::vector<uint8_t>(content.begin(), content.end()));
            return shared::ToolResult::ok();
        } else if (!input.content.empty()) {
            // Overwrite mode
            ctx.host.writeFile(absolutePath, std::vector<uint8_t>(input.content.begin(), input.content.end()));
            return shared::ToolResult::ok();
        } else {
            return shared::ToolResult::fail("Missing content or replacement strings");
        }
    } catch (const std::exception& e) {
        return shared::ToolResult::fail(e.what());
    }
}

}

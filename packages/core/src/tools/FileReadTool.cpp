#include "tools/FileReadTool.hpp"
#include "agents/Agent.hpp"
#include "utils/FSUtil.hpp"
#include <filesystem>
#include <fstream>
#include <sstream>

namespace firmius::core {
using namespace firmius::shared;

shared::ToolMetadata FileReadTool::getMetadata() const {
    return {"file_read", "Read a file from the host filesystem", ToolScope::FilesystemRead};
}

std::shared_ptr<shared::JSONSchema> FileReadTool::getSchema() const {
    return zObject({
        {"path", zString()->describe("Absolute or relative path to the file")},
        {"start_line", zInteger()->describe("Line number to start reading from (1-indexed)")->setOptional()},
        {"end_line", zInteger()->describe("Line number to end reading at (inclusive)")->setOptional()}
    })->required({"path"});
}

shared::ToolResult FileReadTool::execute(const FileReadInput& input, shared::ToolContext& ctx) {
    std::string absolutePath = ctx.agent.resolvePath(input.path);

    // Security check
    bool allowed = false;
    for (const auto& p : ctx.agent.getContext().permissions.allowedPaths) {
        if (FSUtil::isSubpath(absolutePath, p)) {
            allowed = true;
            break;
        }
    }
    if (!allowed && !ctx.agent.getContext().permissions.allowOutsideCwd) {
        return shared::ToolResult::fail("Access denied: path outside allowed directories: " + absolutePath);
    }

    try {
        std::ifstream file(absolutePath);
        if (!file.is_open()) {
            return shared::ToolResult::fail("Could not open file: " + absolutePath);
        }

        std::string line;
        std::string sliced;
        int current = 1;
        bool reachedEnd = false;
        while (std::getline(file, line)) {
            if (current >= input.start_line && (input.end_line == -1 || current <= input.end_line)) {
                sliced += line + "\n";
            }
            if (input.end_line != -1 && current >= input.end_line) {
                // Peek to see if there's anything else in the file
                file.peek();
                reachedEnd = file.eof();
                current++;
                break;
            }
            current++;
            if (file.eof()) reachedEnd = true;
        }
        if (file.eof()) reachedEnd = true;

        // Mark file as read if the entire file was read (start_line == 1 and we reached EOF)
        if (input.start_line == 1 && reachedEnd) {
            ctx.agent.markFileAsRead(absolutePath);
        }
        
        rapidjson::Document res;
        res.SetObject();
        res.AddMember("content", rapidjson::Value(sliced.c_str(), res.GetAllocator()).Move(), res.GetAllocator());
        return shared::ToolResult::ok(res);
    } catch (const std::exception& e) {
        return shared::ToolResult::fail(e.what());
    }
}

}

#include "tools/FileReadTool.hpp"
#include "agents/Agent.hpp"
#include "agents/AgentPermissionChecks.hpp"
#include "utils/FSUtil.hpp"
#include "utils/Hashline.hpp"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace firmius::core {
using namespace firmius::shared;

shared::ToolMetadata FileReadTool::getMetadata() const {
  return {"file_read", "Read a file from the host filesystem",
          ToolScope::FilesystemRead};
}

std::shared_ptr<shared::JSONSchema> FileReadTool::getSchema() const {
  return zObject(
             {{"path",
               zString()->describe("Absolute or relative path to the file")},
              {"start_line",
               zInteger()
                   ->describe("Line number to start reading from (1-indexed)")
                   ->setOptional()},
              {"end_line",
               zInteger()
                   ->describe("Line number to end reading at (inclusive)")
                   ->setOptional()}})
      ->required({"path"});
}

shared::ToolResult FileReadTool::execute(const FileReadInput &input,
                                         shared::ToolContext &ctx) {
  std::string absolutePath =
      ctx.agent.getEnvironment()->getWorkspace().resolvePath(input.path);

  try {
    ctx.agent.getPermissions()->validatePathAccess(
        absolutePath, firmius::shared::AccessMode::READ);
    auto data = ctx.host.readFile(absolutePath);
    std::string content(data.begin(), data.end());
    std::stringstream ss(content);

    std::vector<std::string> selectedLines;
    std::string line;
    int current = 1;
    bool reachedEnd = false;
    int lines_taken = 0;
    while (std::getline(ss, line)) {
      if (current >= input.start_line &&
          (input.end_line == -1 || current <= input.end_line)) {
        lines_taken++;
        selectedLines.push_back(line);
      }
      if (input.end_line != -1 && current >= input.end_line) {
        reachedEnd = (ss.peek() == EOF);
        current++;
        break;
      }
      current++;
      if (ss.eof())
        reachedEnd = true;
    }
    if (ss.eof())
      reachedEnd = true;

    // Old fully read check logic removed

    rapidjson::Document res;
    res.SetObject();
    auto &alloc = res.GetAllocator();
    int normalized_start = std::max(1, input.start_line);
    int normalized_end = normalized_start + lines_taken - 1;
    if (lines_taken == 0)
      normalized_end = normalized_start - 1;
    bool read_full = reachedEnd && input.start_line <= 1;

    if (read_full) {
      ctx.agent.getEnvironment()->getWorkspace().markFileAsFullyRead(
          absolutePath);
    } else {
      ctx.agent.getEnvironment()->getWorkspace().recordFileRead(
          absolutePath, normalized_start, normalized_end, reachedEnd);
    }

    std::string enhancedContent;
    if (!selectedLines.empty()) {
      for (std::size_t i = 0; i < selectedLines.size(); ++i) {
        enhancedContent +=
            std::to_string(input.start_line) + "|" + selectedLines[i];
        if (i + 1 < selectedLines.size()) {
          enhancedContent += '\n';
        }
      }
    }

    res.AddMember("content",
                  rapidjson::Value(enhancedContent.c_str(), alloc).Move(),
                  alloc);
    res.AddMember("line_start", normalized_start, alloc);
    res.AddMember("line_end", normalized_end, alloc);
    res.AddMember("lines_read", lines_taken, alloc);
    res.AddMember("read_full", read_full, alloc);
    res.AddMember("reached_end", reachedEnd, alloc);
    res.AddMember("watch_state", rapidjson::Value("updated", alloc).Move(),
                  alloc);
    res.AddMember("watch_scope",
                  rapidjson::Value(read_full ? "full" : "range", alloc).Move(),
                  alloc);
    return shared::ToolResult::ok(res);
  } catch (const std::exception &e) {
    return shared::ToolResult::fail(e.what());
  }
}

} // namespace firmius::core

#include "tools/FileReadTool.hpp"
#include "agents/Agent.hpp"
#include "agents/AgentPermissionChecks.hpp"
#include "utils/FSUtil.hpp"
#include "utils/Hashline.hpp"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>

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
  std::string absolutePath = ctx.agent.resolvePath(input.path);

  try {
    ctx.agent.getPermissionChecks().validatePathAccess(absolutePath);
    auto data = ctx.host.readFile(absolutePath);
    std::string content(data.begin(), data.end());
    std::stringstream ss(content);

    std::string line;
    std::string sliced;
    int current = 1;
    bool reachedEnd = false;
    int lines_taken = 0;
    while (std::getline(ss, line)) {
      if (current >= input.start_line &&
          (input.end_line == -1 || current <= input.end_line)) {
        sliced += line + "\n";
        lines_taken++;
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

    // Mark file as read if the entire file was read (start_line == 1 and we
    // reached EOF)
    if (input.start_line == 1 && reachedEnd) {
      ctx.agent.markFileAsRead(absolutePath);
    }

    std::string enhancedContent =
        shared::utils::HashlineReadEnhancer::enhance(sliced);

    rapidjson::Document res;
    res.SetObject();
    res.AddMember(
        "content",
        rapidjson::Value(enhancedContent.c_str(), res.GetAllocator()).Move(),
        res.GetAllocator());
    int normalized_start = std::max(1, input.start_line);
    int normalized_end = normalized_start + lines_taken - 1;
    if (lines_taken == 0)
      normalized_end = normalized_start - 1;
    bool read_full = reachedEnd && input.start_line <= 1 &&
                     (input.end_line == -1 || normalized_end <= input.end_line);
    res.AddMember("line_start", normalized_start, res.GetAllocator());
    res.AddMember("line_end", normalized_end, res.GetAllocator());
    res.AddMember("lines_read", lines_taken, res.GetAllocator());
    res.AddMember("read_full", read_full, res.GetAllocator());
    return shared::ToolResult::ok(res);
  } catch (const std::exception &e) {
    return shared::ToolResult::fail(e.what());
  }
}

} // namespace firmius::core

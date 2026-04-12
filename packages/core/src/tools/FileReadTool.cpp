#include "tools/FileReadTool.hpp"
#include "agents/Agent.hpp"
#include "agents/AgentPermissionChecks.hpp"
#include "agents/PurposeLoader.hpp"
#include "utils/FSUtil.hpp"
#include "utils/Hashline.hpp"
#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace firmius::core {
using namespace firmius::shared;

// Maximum file size for file_read tool (2MB default)
static constexpr std::uint64_t MAX_FILE_SIZE_BYTES = 2ULL * 1024 * 1024;

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
    PurposeLoader::loadDiscoveredAgentsForPath(ctx.agent.getMutableContext(),
                                               absolutePath);

    // Check file size before reading to prevent bad_alloc
    std::uint64_t fileSize = 0;
    try {
      fileSize = std::filesystem::file_size(absolutePath);
    } catch (const std::exception &) {
      // If we can't determine size, proceed with read (will be bounded below)
    }

    bool truncated = false;
    std::vector<uint8_t> data;
    if (fileSize > MAX_FILE_SIZE_BYTES) {
      // Read only up to the limit
      std::ifstream file(absolutePath, std::ios::binary);
      if (!file.is_open())
        throw std::runtime_error("Could not open file: " + absolutePath);
      data.resize(MAX_FILE_SIZE_BYTES);
      file.read(reinterpret_cast<char *>(data.data()), MAX_FILE_SIZE_BYTES);
      data.resize(static_cast<std::size_t>(file.gcount()));
      truncated = true;
    } else {
      data = ctx.host.readFile(absolutePath);
    }
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
            utils::Hashline::formatLine(input.start_line +
                                            static_cast<int>(i),
                                        selectedLines[i]);
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
    res.AddMember("watch_scope",
                  rapidjson::Value(read_full ? "full" : "range", alloc).Move(),
                  alloc);
    res.AddMember("watch_state", rapidjson::Value("updated", alloc).Move(),
                  alloc);
    if (truncated) {
      res.AddMember("truncated", true, alloc);
      std::string warning = "File content truncated: file size (" +
                            std::to_string(fileSize) + " bytes) exceeds limit of " +
                            std::to_string(MAX_FILE_SIZE_BYTES) + " bytes.";
      res.AddMember("warning",
                    rapidjson::Value(warning.c_str(), alloc).Move(), alloc);
    }
    return shared::ToolResult::ok(res);
  } catch (const std::exception &e) {
    return shared::ToolResult::fail(e.what());
  }
}

} // namespace firmius::core

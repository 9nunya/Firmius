#include "tools/ReadTool.hpp"

#include "agents/Agent.hpp"
#include "agents/AgentPermissionChecks.hpp"
#include "agents/PurposeLoader.hpp"
#include "utils/SpillIfLarge.hpp"
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace firmius::core {
using namespace firmius::shared;

namespace {

static constexpr std::uint64_t MAX_FILE_SIZE_BYTES = 2ULL * 1024 * 1024;

} // namespace

shared::ToolMetadata ReadTool::getMetadata() const {
  return {
      "Read",
      R"(Read a text file (optionally line-ranged). Returns numbered lines.

USAGE GUIDANCE:
- Prefer this tool over guessing repository state.
- Always pass paths relative to the workspace root when possible.
- Use Read before making edits so you anchor your changes to exact file contents.
- Treat returned output as authoritative: if it says truncated=true, refine the query.

SECURITY / PERMISSIONS:
- Requires filesystem READ access to the resolved path.
- Binary/huge files may be truncated for safety.
)",
      shared::ToolScope::FilesystemRead};
}

std::shared_ptr<shared::JSONSchema> ReadTool::getSchema() const {
  return shared::zObject({
      {"path",
       shared::zString()
           ->describe(
               "The file to read. Provide a workspace-relative path when possible. "
               "Security: must be readable under current permissions. Relative paths are resolved against the workspace root.")},
      {"start_line",
       shared::zInteger()
           ->setOptional()
           ->describe(
               "1-based starting line to include. Default: 1. If provided with end_line, returns only that line range.")},
      {"end_line",
       shared::zInteger()
           ->setOptional()
           ->describe(
               "1-based ending line to include (inclusive). Default: -1 (read to end of file).")},
  });
}

shared::ToolResult ReadTool::execute(const rapidjson::Value &input, shared::ToolContext &ctx) {
  std::string path;
  if (input.HasMember("path") && input["path"].IsString()) {
    path = input["path"].GetString();
  } else {
    return shared::ToolResult::fail("Missing required field: path");
  }

  int start_line = 1;
  if (input.HasMember("start_line") && input["start_line"].IsInt()) {
    start_line = input["start_line"].GetInt();
  }

  int end_line = -1;
  if (input.HasMember("end_line") && input["end_line"].IsInt()) {
    end_line = input["end_line"].GetInt();
  }

  std::string absolutePath =
      ctx.agent.getEnvironment()->getWorkspace().resolvePath(path);

  try {
    ctx.agent.getPermissions()->validatePathAccess(
        absolutePath, firmius::shared::AccessMode::READ);
    PurposeLoader::loadDiscoveredAgentsForPath(ctx.agent.getMutableContext(),
                                               absolutePath);

    std::uint64_t fileSize = 0;
    try {
      fileSize = std::filesystem::file_size(absolutePath);
    } catch (const std::exception &) {
    }

    bool truncated = false;
    std::vector<uint8_t> data;
    if (fileSize > MAX_FILE_SIZE_BYTES) {
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
      if (current >= start_line &&
          (end_line == -1 || current <= end_line)) {
        lines_taken++;
        selectedLines.push_back(line);
      }
      if (end_line != -1 && current >= end_line) {
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
    int normalized_start = std::max(1, start_line);
    int normalized_end = normalized_start + lines_taken - 1;
    if (lines_taken == 0)
      normalized_end = normalized_start - 1;
    bool read_full = reachedEnd && start_line <= 1;

    if (read_full) {
      ctx.agent.getEnvironment()->getWorkspace().markFileAsFullyRead(
          absolutePath);
    } else {
      ctx.agent.getEnvironment()->getWorkspace().recordFileRead(
          absolutePath, normalized_start, normalized_end, reachedEnd);
    }

    // Token-waste pass 5: count total lines once so we can emit a single
    // `range: "<start>-<end>/<total>"` field instead of six derivable
    // counters. For partially-read (truncated) files, total reflects what
    // we actually loaded into memory; the truncated flag still flags the
    // overall file-too-big case.
    int totalLines = 0;
    for (char c : content) if (c == '\n') ++totalLines;
    if (!content.empty() && content.back() != '\n') ++totalLines;

    std::string enhancedContent;
    if (!selectedLines.empty()) {
      for (std::size_t i = 0; i < selectedLines.size(); ++i) {
        enhancedContent += std::to_string(start_line + static_cast<int>(i));
        enhancedContent += '|';
        enhancedContent += selectedLines[i];
        if (i + 1 < selectedLines.size()) {
          enhancedContent += '\n';
        }
      }
    }

    // Token-waste pass 5: prose-first read result. `range` collapses
    // line_start/line_end/lines_read/reached_end/read_full into one
    // string; watch_scope/watch_state are bookkeeping the model rarely
    // reads — dropped. Spill the line-prefixed content when very large.
    constexpr std::size_t kReadSpillThreshold = 1ull * 1024 * 1024;  // 1 MB
    constexpr std::size_t kReadTailBytes = 4 * 1024;
    std::string rangeStr;
    if (lines_taken == 0) {
      rangeStr = "0-0/" + std::to_string(totalLines);
    } else {
      rangeStr = std::to_string(normalized_start) + "-" +
                 std::to_string(normalized_end) + "/" +
                 std::to_string(totalLines);
    }

    if (enhancedContent.size() > kReadSpillThreshold) {
      auto spill = shared::utils::spillIfLarge(
          enhancedContent, kReadSpillThreshold,
          "firmius_read", kReadTailBytes);
      std::ostringstream prose;
      prose << "Read " << path << " (" << rangeStr
            << " lines, " << spill.totalBytes
            << " B); content spilled to " << spill.refPath
            << " (showing last " << spill.tail.size() << " B).";
      const std::string proseStr = prose.str();
      res.AddMember(
          "result",
          rapidjson::Value(proseStr.c_str(),
                           static_cast<rapidjson::SizeType>(proseStr.size()),
                           alloc).Move(),
          alloc);
      res.AddMember(
          "tail",
          rapidjson::Value(spill.tail.c_str(),
                           static_cast<rapidjson::SizeType>(spill.tail.size()),
                           alloc).Move(),
          alloc);
      res.AddMember("ref",
                    rapidjson::Value(spill.refPath.c_str(), alloc).Move(),
                    alloc);
    } else {
      res.AddMember("content",
                    rapidjson::Value(enhancedContent.c_str(), alloc).Move(),
                    alloc);
    }
    res.AddMember("range",
                  rapidjson::Value(rangeStr.c_str(),
                                   static_cast<rapidjson::SizeType>(rangeStr.size()),
                                   alloc).Move(),
                  alloc);
    if (truncated) {
      // Hard-cap notice for files exceeding MAX_FILE_SIZE_BYTES (2 MB).
      // Distinct from the normal-spill case above.
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

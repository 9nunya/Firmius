#include "tools/FileEditTool.hpp"
#include "agents/Agent.hpp"
#include "agents/AgentPermissionChecks.hpp"
#include "utils/FSUtil.hpp"
#include "utils/StringUtil.hpp"
#include <filesystem>
#include <iostream>
#include <rapidjson/document.h>

namespace firmius::core {
using namespace firmius::shared;

shared::ToolMetadata FileEditTool::getMetadata() const {
  return {"file_edit", "Edit or overwrite a file on the host filesystem",
          ToolScope::FilesystemWrite};
}

std::shared_ptr<shared::JSONSchema> FileEditTool::getSchema() const {
  return zObject(
             {{"path",
               zString()->describe("Absolute or relative path to the file")},
              {"content", zString()
                              ->describe("Full content to write to the file")
                              ->setOptional()},
              {"old_string", zString()
                                 ->describe("Exact substring to replace")
                                 ->setOptional()},
              {"new_string",
               zString()->describe("New content to substitute")->setOptional()},
              {"replace_all",
               zBoolean()
                   ->describe("If true, replaces all occurrences of old_string")
                   ->setOptional()},
              {"fuzzy_threshold", zNumber()
                                      ->describe("Similarity threshold (0.0 to "
                                                 "1.0) for matching old_string")
                                      ->setOptional()}})
      ->required({"path"});
}

shared::ToolResult FileEditTool::execute(const FileEditInput &input,
                                         shared::ToolContext &ctx) {
  std::string absolutePath = ctx.agent.resolvePath(input.path);

  // Only require reading if file already exists (skip for new files)
  if (ctx.host.exists(absolutePath) && !ctx.agent.hasReadFile(absolutePath)) {
    return shared::ToolResult::fail(
        "You MUST READ the ENTIRE file before making any edits to ensure "
        "complete context and avoid breaking dependencies or logic. Use "
        "'file_read' on '" +
        input.path +
        "' first. Omit 'start_line' and 'end_line' arguments to read the "
        "entire file.");
  }

  ctx.agent.getPermissionChecks().validatePathAccess(absolutePath);

  try {
    if (!input.old_string.empty() && !input.new_string.empty()) {
      // Replace mode
      auto data = ctx.host.readFile(absolutePath);
      std::string content(data.begin(), data.end());

      std::vector<size_t> matchIndices;
      if (input.fuzzy_threshold < 1.0f) {
        matchIndices = StringUtil::findFuzzy(content, input.old_string,
                                             input.fuzzy_threshold);
      } else {
        size_t pos = content.find(input.old_string);
        while (pos != std::string::npos) {
          matchIndices.push_back(pos);
          pos = content.find(input.old_string, pos + input.old_string.length());
        }
      }

      if (matchIndices.empty()) {
        return shared::ToolResult::fail(
            "old_string not found in file (threshold=" +
            std::to_string(input.fuzzy_threshold) + ")");
      }

      if (matchIndices.size() > 1 && !input.replace_all) {
        return shared::ToolResult::fail(
            "Multiple matches found for old_string, but replace_all is false. "
            "Matches: " +
            std::to_string(matchIndices.size()));
      }

      size_t occurrences = 0;
      // Iterate backwards to keep indices valid during replacement
      std::reverse(matchIndices.begin(), matchIndices.end());

      // For non-replace_all, we already checked that size is 1 or we handle
      // multiple as error above Actually, if replace_all is false and multiple
      // matches, we failed. If replace_all is false and 1 match, we replace it.
      // If replace_all is true, we replace all.

      for (size_t pos : matchIndices) {
        // If fuzzy, we need to know the length of the match. Sliding window
        // uses pattern length.
        size_t matchLen = input.old_string.length();
        content.replace(pos, matchLen, input.new_string);
        occurrences++;
        if (!input.replace_all)
          break;
      }

      ctx.host.writeFile(absolutePath,
                         std::vector<uint8_t>(content.begin(), content.end()));

      rapidjson::Document resDoc;
      resDoc.SetObject();
      resDoc.AddMember("occurrences", static_cast<uint32_t>(occurrences),
                       resDoc.GetAllocator());
      return shared::ToolResult::ok(resDoc);
    } else if (!input.content.empty()) {
      // Overwrite mode
      ctx.host.writeFile(
          absolutePath,
          std::vector<uint8_t>(input.content.begin(), input.content.end()));
      return shared::ToolResult::ok("{ message: \"Overwrote " + absolutePath +
                                    " successfully with new content.\" }");
    } else {
      return shared::ToolResult::fail("Missing content or replacement strings");
    }
  } catch (const std::exception &e) {
    return shared::ToolResult::fail(e.what());
  }
}

} // namespace firmius::core

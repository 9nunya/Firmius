#include "tools/FileEditTool.hpp"
#include "agents/Agent.hpp"
#include "utils/Hashline.hpp"
#include "utils/StringUtil.hpp"
#include <algorithm>
#include <rapidjson/document.h>
#include <sstream>
#include <stdexcept>

namespace firmius::core {
using namespace firmius::shared;

namespace {

constexpr int kAnchorSearchWindow = 3;

struct FileBuffer {
  std::vector<std::string> lines;
  bool hadTrailingNewline = false;
};

struct ResolvedAnchor {
  int lineIndex = -1;
  bool relocated = false;
};

struct NormalizedEdit {
  std::string op;
  int startIndex = 0;
  int endIndex = 0;
  std::vector<std::string> newLines;
  std::string description;
  bool relocated = false;
};

std::vector<std::string> readStringArray(const rapidjson::Value &json,
                                         const char *key) {
  std::vector<std::string> values;
  if (!json.HasMember(key) || !json[key].IsArray()) {
    return values;
  }
  for (const auto &value : json[key].GetArray()) {
    if (value.IsString()) {
      values.emplace_back(value.GetString());
    }
  }
  return values;
}

FileBuffer splitFileContent(const std::string &content) {
  FileBuffer buffer;
  buffer.hadTrailingNewline = !content.empty() && content.back() == '\n';

  std::stringstream ss(content);
  std::string line;
  while (std::getline(ss, line)) {
    buffer.lines.push_back(line);
  }
  return buffer;
}

std::string joinFileContent(const FileBuffer &buffer) {
  if (buffer.lines.empty()) {
    return "";
  }

  std::string result;
  for (size_t i = 0; i < buffer.lines.size(); ++i) {
    result += buffer.lines[i];
    if (i + 1 < buffer.lines.size() || buffer.hadTrailingNewline) {
      result += '\n';
    }
  }
  return result;
}

ResolvedAnchor resolveAnchor(const std::vector<std::string> &lines,
                             const std::string &anchorText) {
  if (anchorText.empty()) {
    throw std::runtime_error("Missing required anchor");
  }

  auto parsed = utils::Hashline::parseAnchor(anchorText);
  if (!parsed) {
    throw std::runtime_error("Malformed anchor '" + anchorText +
                             "'. Expected lineNumber#hash.");
  }

  const int expectedIndex = parsed->lineNumber - 1;
  if (expectedIndex >= 0 &&
      expectedIndex < static_cast<int>(lines.size()) &&
      utils::Hashline::verifyAnchor(parsed->hash, lines[expectedIndex])) {
    return {expectedIndex, false};
  }

  std::vector<int> matches;
  const int minIndex = std::max(0, expectedIndex - kAnchorSearchWindow);
  const int maxIndex =
      std::min(static_cast<int>(lines.size()) - 1, expectedIndex + kAnchorSearchWindow);

  for (int index = minIndex; index <= maxIndex; ++index) {
    if (index == expectedIndex) {
      continue;
    }
    if (utils::Hashline::verifyAnchor(parsed->hash, lines[index])) {
      matches.push_back(index);
    }
  }

  if (matches.size() == 1) {
    return {matches.front(), true};
  }

  if (matches.size() > 1) {
    throw std::runtime_error("Anchor '" + anchorText +
                             "' matched multiple nearby lines. Reread the file "
                             "and try again.");
  }

  throw std::runtime_error("Anchor '" + anchorText +
                           "' did not match the current file. Reread the file "
                           "and try again.");
}

bool editsConflict(const NormalizedEdit &left, const NormalizedEdit &right) {
  const bool leftInsertion = left.startIndex == left.endIndex;
  const bool rightInsertion = right.startIndex == right.endIndex;

  if (leftInsertion && rightInsertion) {
    return left.startIndex == right.startIndex;
  }

  if (leftInsertion) {
    return left.startIndex >= right.startIndex &&
           left.startIndex <= right.endIndex;
  }
  if (rightInsertion) {
    return right.startIndex >= left.startIndex &&
           right.startIndex <= left.endIndex;
  }

  return left.startIndex < right.endIndex && right.startIndex < left.endIndex;
}

std::vector<NormalizedEdit> normalizeEdits(const std::vector<FileEditOperationInput> &edits,
                                           const std::vector<std::string> &lines) {
  std::vector<NormalizedEdit> normalized;
  normalized.reserve(edits.size());

  for (const auto &edit : edits) {
    if (edit.op == "replace_range") {
      auto start = resolveAnchor(lines, edit.start_anchor);
      auto end = resolveAnchor(lines, edit.end_anchor);
      if (start.lineIndex > end.lineIndex) {
        throw std::runtime_error("Invalid replace_range: start anchor '" +
                                 edit.start_anchor +
                                 "' resolved after end anchor '" +
                                 edit.end_anchor + "'.");
      }
      normalized.push_back({"replace_range",
                            start.lineIndex,
                            end.lineIndex + 1,
                            edit.new_lines,
                            "replace " + edit.start_anchor + "..." +
                                edit.end_anchor,
                            start.relocated || end.relocated});
      continue;
    }

    if (edit.op == "delete_range") {
      auto start = resolveAnchor(lines, edit.start_anchor);
      auto end = resolveAnchor(lines, edit.end_anchor);
      if (start.lineIndex > end.lineIndex) {
        throw std::runtime_error("Invalid delete_range: start anchor '" +
                                 edit.start_anchor +
                                 "' resolved after end anchor '" +
                                 edit.end_anchor + "'.");
      }
      normalized.push_back({"delete_range",
                            start.lineIndex,
                            end.lineIndex + 1,
                            {},
                            "delete " + edit.start_anchor + "..." +
                                edit.end_anchor,
                            start.relocated || end.relocated});
      continue;
    }

    if (edit.op == "insert_after") {
      auto anchor = resolveAnchor(lines, edit.anchor);
      normalized.push_back({"insert_after",
                            anchor.lineIndex + 1,
                            anchor.lineIndex + 1,
                            edit.new_lines,
                            "insert after " + edit.anchor,
                            anchor.relocated});
      continue;
    }

    if (edit.op == "insert_before") {
      auto anchor = resolveAnchor(lines, edit.anchor);
      normalized.push_back({"insert_before",
                            anchor.lineIndex,
                            anchor.lineIndex,
                            edit.new_lines,
                            "insert before " + edit.anchor,
                            anchor.relocated});
      continue;
    }

    throw std::runtime_error("Malformed edit op '" + edit.op +
                             "'. Expected replace_range, insert_after, "
                             "insert_before, or delete_range.");
  }

  std::stable_sort(normalized.begin(), normalized.end(),
                   [](const NormalizedEdit &left, const NormalizedEdit &right) {
                     if (left.startIndex != right.startIndex) {
                       return left.startIndex < right.startIndex;
                     }
                     return left.endIndex < right.endIndex;
                   });

  for (size_t i = 1; i < normalized.size(); ++i) {
    if (editsConflict(normalized[i - 1], normalized[i])) {
      throw std::runtime_error("Overlapping edits are not allowed: '" +
                               normalized[i - 1].description + "' conflicts "
                               "with '" + normalized[i].description + "'.");
    }
  }

  return normalized;
}

ToolResult executeLegacyReplace(const FileEditInput &input,
                                const std::string &absolutePath,
                                shared::ToolContext &ctx) {
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
        "Legacy old_string not found in file (threshold=" +
        std::to_string(input.fuzzy_threshold) + ")");
  }

  if (matchIndices.size() > 1 && !input.replace_all) {
    return shared::ToolResult::fail(
        "Legacy old_string matched multiple locations. Use Hashline edits "
        "instead or set replace_all for compatibility mode.");
  }

  std::reverse(matchIndices.begin(), matchIndices.end());
  size_t occurrences = 0;
  for (size_t pos : matchIndices) {
    content.replace(pos, input.old_string.length(), input.new_string);
    ++occurrences;
    if (!input.replace_all) {
      break;
    }
  }

  ctx.host.writeFile(absolutePath,
                     std::vector<uint8_t>(content.begin(), content.end()));

  rapidjson::Document resDoc;
  resDoc.SetObject();
  resDoc.AddMember("path",
                   rapidjson::Value(input.path.c_str(), resDoc.GetAllocator())
                       .Move(),
                   resDoc.GetAllocator());
  resDoc.AddMember(
      "mode",
      rapidjson::Value("legacy_string_replace", resDoc.GetAllocator()).Move(),
      resDoc.GetAllocator());
  resDoc.AddMember("occurrences", static_cast<uint32_t>(occurrences),
                   resDoc.GetAllocator());
  return shared::ToolResult::ok(resDoc);
}

} // namespace

shared::ToolMetadata FileEditTool::getMetadata() const {
  return {"file_edit",
          "Edit a file on the host filesystem using Hashline anchors",
          ToolScope::FilesystemWrite};
}

std::shared_ptr<shared::JSONSchema> FileEditTool::getSchema() const {
  auto editSchema =
      zObject({{"op",
                zEnum({"replace_range", "insert_after", "insert_before",
                       "delete_range"})
                    ->describe("Hashline edit operation type")},
               {"start_anchor",
                zString()
                    ->describe(
                        "Start anchor in lineNumber#hash form for range edits")
                    ->setOptional()},
               {"end_anchor",
                zString()
                    ->describe(
                        "End anchor in lineNumber#hash form for range edits")
                    ->setOptional()},
               {"anchor",
                zString()
                    ->describe(
                        "Single anchor in lineNumber#hash form for insert edits")
                    ->setOptional()},
               {"new_lines",
                zArray(zString())
                    ->describe("Replacement or inserted lines without trailing "
                               "newline characters")
                    ->setOptional()}});

  return zObject(
             {{"path",
               zString()->describe("Absolute or relative path to the file")},
              {"edits",
               zArray(editSchema)
                   ->describe("Hashline-anchored edit operations. Prefer this "
                              "for modifying existing files.")
                   ->setOptional()},
              {"content", zString()
                              ->describe("Whole-file content for overwrite mode")
                              ->setOptional()},
              {"old_string",
               zString()
                   ->describe("Legacy compatibility only. Prefer edits[].")
                   ->setOptional()},
              {"new_string",
               zString()
                   ->describe("Legacy compatibility only. Prefer edits[].")
                   ->setOptional()},
              {"replace_all",
               zBoolean()
                   ->describe("Legacy compatibility only for old_string mode")
                   ->setOptional()},
              {"fuzzy_threshold",
               zNumber()
                   ->describe("Legacy compatibility only for old_string mode")
                   ->setOptional()}})
      ->required({"path"});
}

FileEditInput FileEditTool::transform(const rapidjson::Value &json) {
  FileEditInput input;

  if (json.HasMember("path") && json["path"].IsString()) {
    input.path = json["path"].GetString();
  }
  if (json.HasMember("content") && json["content"].IsString()) {
    input.has_content = true;
    input.content = json["content"].GetString();
  }
  if (json.HasMember("old_string") && json["old_string"].IsString()) {
    input.has_old_string = true;
    input.old_string = json["old_string"].GetString();
  }
  if (json.HasMember("new_string") && json["new_string"].IsString()) {
    input.has_new_string = true;
    input.new_string = json["new_string"].GetString();
  }
  if (json.HasMember("replace_all") && json["replace_all"].IsBool()) {
    input.replace_all = json["replace_all"].GetBool();
  }
  if (json.HasMember("fuzzy_threshold") && json["fuzzy_threshold"].IsNumber()) {
    input.fuzzy_threshold = json["fuzzy_threshold"].GetFloat();
  }
  if (json.HasMember("edits") && json["edits"].IsArray()) {
    for (const auto &value : json["edits"].GetArray()) {
      if (!value.IsObject()) {
        continue;
      }

      FileEditOperationInput edit;
      if (value.HasMember("op") && value["op"].IsString()) {
        edit.op = value["op"].GetString();
      }
      if (value.HasMember("start_anchor") && value["start_anchor"].IsString()) {
        edit.start_anchor = value["start_anchor"].GetString();
      }
      if (value.HasMember("end_anchor") && value["end_anchor"].IsString()) {
        edit.end_anchor = value["end_anchor"].GetString();
      }
      if (value.HasMember("anchor") && value["anchor"].IsString()) {
        edit.anchor = value["anchor"].GetString();
      }
      edit.new_lines = readStringArray(value, "new_lines");
      input.edits.push_back(std::move(edit));
    }
  }

  return input;
}

shared::ToolResult FileEditTool::execute(const FileEditInput &input,
                                         shared::ToolContext &ctx) {
  std::string absolutePath =
      ctx.agent.getEnvironment()->getWorkspace().resolvePath(input.path);

  const bool fileExists = ctx.host.exists(absolutePath);
  const bool hasAnchorEdits = !input.edits.empty();
  const bool hasOverwrite = input.has_content;
  const bool hasLegacyReplace = input.has_old_string && input.has_new_string;

  if (fileExists &&
      !ctx.agent.getEnvironment()->getWorkspace().hasFullyReadFile(
          absolutePath)) {
    return shared::ToolResult::fail(
        "You MUST READ the ENTIRE file before editing it. Use 'file_read' on '" +
        input.path +
        "' first, then reference the returned Hashline anchors in file_edit.");
  }

  try {
    ctx.agent.getPermissions()->validatePathAccess(absolutePath,
                                                   AccessMode::WRITE);

    if (hasAnchorEdits) {
      if (!fileExists) {
        return shared::ToolResult::fail(
            "Hashline edits require an existing file. Use content to create a "
            "new file.");
      }

      auto data = ctx.host.readFile(absolutePath);
      std::string content(data.begin(), data.end());
      FileBuffer buffer = splitFileContent(content);

      auto normalized = normalizeEdits(input.edits, buffer.lines);
      int removedLines = 0;
      int addedLines = 0;
      int relocatedAnchors = 0;

      for (const auto &edit : normalized) {
        removedLines += edit.endIndex - edit.startIndex;
        addedLines += static_cast<int>(edit.newLines.size());
        if (edit.relocated) {
          ++relocatedAnchors;
        }
      }

      for (auto it = normalized.rbegin(); it != normalized.rend(); ++it) {
        auto begin = buffer.lines.begin() + it->startIndex;
        auto end = buffer.lines.begin() + it->endIndex;
        buffer.lines.erase(begin, end);
        buffer.lines.insert(buffer.lines.begin() + it->startIndex,
                            it->newLines.begin(), it->newLines.end());
      }

      std::string updated = joinFileContent(buffer);
      ctx.host.writeFile(absolutePath,
                         std::vector<uint8_t>(updated.begin(), updated.end()));

      rapidjson::Document resDoc;
      resDoc.SetObject();
      auto &alloc = resDoc.GetAllocator();
      resDoc.AddMember("path",
                       rapidjson::Value(input.path.c_str(), alloc).Move(),
                       alloc);
      resDoc.AddMember("mode",
                       rapidjson::Value("hashline_edits", alloc).Move(),
                       alloc);
      resDoc.AddMember("applied_edits",
                       static_cast<uint32_t>(normalized.size()), alloc);
      resDoc.AddMember("removed_lines", removedLines, alloc);
      resDoc.AddMember("added_lines", addedLines, alloc);
      resDoc.AddMember("relocated_anchors", relocatedAnchors, alloc);

      rapidjson::Value operations(rapidjson::kArrayType);
      for (const auto &edit : normalized) {
        rapidjson::Value op(rapidjson::kObjectType);
        op.AddMember("op", rapidjson::Value(edit.op.c_str(), alloc).Move(),
                     alloc);
        op.AddMember(
            "description",
            rapidjson::Value(edit.description.c_str(), alloc).Move(), alloc);
        op.AddMember("start_line", edit.startIndex + 1, alloc);
        op.AddMember("end_line", edit.endIndex, alloc);
        op.AddMember("new_line_count",
                     static_cast<uint32_t>(edit.newLines.size()), alloc);
        op.AddMember("relocated", edit.relocated, alloc);
        operations.PushBack(op, alloc);
      }
      resDoc.AddMember("operations", operations, alloc);
      return shared::ToolResult::ok(resDoc);
    }

    if (hasOverwrite) {
      ctx.host.writeFile(
          absolutePath,
          std::vector<uint8_t>(input.content.begin(), input.content.end()));

      rapidjson::Document resDoc;
      resDoc.SetObject();
      auto &alloc = resDoc.GetAllocator();
      resDoc.AddMember("path",
                       rapidjson::Value(input.path.c_str(), alloc).Move(),
                       alloc);
      resDoc.AddMember("mode",
                       rapidjson::Value("overwrite", alloc).Move(), alloc);
      resDoc.AddMember("bytes_written",
                       static_cast<uint32_t>(input.content.size()), alloc);
      return shared::ToolResult::ok(resDoc);
    }

    if (hasLegacyReplace) {
      return executeLegacyReplace(input, absolutePath, ctx);
    }

    return shared::ToolResult::fail(
        "Missing edits, content, or legacy replacement parameters.");
  } catch (const std::exception &e) {
    return shared::ToolResult::fail(e.what());
  }
}

} // namespace firmius::core

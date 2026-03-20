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
constexpr int kPostEditContextLines = 2;

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
  std::vector<std::string> oldLines;
};

std::string joinStrings(const std::vector<std::string> &values,
                        std::string_view separator) {
  std::string result;
  for (size_t i = 0; i < values.size(); ++i) {
    if (i > 0) {
      result += separator;
    }
    result += values[i];
  }
  return result;
}

std::string buildRangeAnchorRequirementMessage(
    const FileEditOperationInput &edit,
    const std::vector<std::string> &missingFields) {
  if (!edit.anchor.empty() && edit.start_anchor.empty() &&
      edit.end_anchor.empty()) {
    return edit.op + " requires both start_anchor and end_anchor; got anchor only";
  }

  if (missingFields.size() == 1) {
    return edit.op + " is missing " + missingFields.front();
  }

  return edit.op + " requires both start_anchor and end_anchor; missing " +
         joinStrings(missingFields, " and ");
}

void requireRangeAnchors(const FileEditOperationInput &edit) {
  std::vector<std::string> missingFields;
  if (edit.start_anchor.empty()) {
    missingFields.emplace_back("start_anchor");
  }
  if (edit.end_anchor.empty()) {
    missingFields.emplace_back("end_anchor");
  }

  if (missingFields.empty()) {
    return;
  }
  std::string message = buildRangeAnchorRequirementMessage(edit, missingFields);
  if (!edit.anchor.empty()) {
    message += ". anchor is not used for range edits";
  }
  throw std::runtime_error(message);
}

void requireSingleAnchor(const FileEditOperationInput &edit) {
  if (!edit.anchor.empty()) {
    return;
  }

  std::vector<std::string> extraFields;
  if (!edit.start_anchor.empty()) {
    extraFields.emplace_back("start_anchor");
  }
  if (!edit.end_anchor.empty()) {
    extraFields.emplace_back("end_anchor");
  }

  std::string message = edit.op + " requires anchor";
  if (!extraFields.empty()) {
    message += "; " + joinStrings(extraFields, " and ") +
               " are not used for insert edits";
  }
  throw std::runtime_error(message);
}

void validateEditOperation(const FileEditOperationInput &edit) {
  if (edit.op == "replace_range" || edit.op == "delete_range") {
    requireRangeAnchors(edit);
    return;
  }

  if (edit.op == "insert_after" || edit.op == "insert_before") {
    requireSingleAnchor(edit);
    return;
  }

  throw std::runtime_error("Malformed edit op '" + edit.op +
                           "'. Expected replace_range, insert_after, "
                           "insert_before, or delete_range.");
}

rapidjson::Value buildHashlineSlice(
    const FileBuffer &buffer, int startLine, int endLine,
    rapidjson::Document::AllocatorType &alloc) {
  rapidjson::Value slice(rapidjson::kObjectType);
  rapidjson::Value lines(rapidjson::kArrayType);
  rapidjson::Value anchors(rapidjson::kArrayType);

  const int fileLineCount = static_cast<int>(buffer.lines.size());
  if (fileLineCount == 0) {
    slice.AddMember("start_line", 0, alloc);
    slice.AddMember("end_line", 0, alloc);
    slice.AddMember("lines", lines, alloc);
    slice.AddMember("anchors", anchors, alloc);
    return slice;
  }

  const int clampedStart = std::clamp(startLine, 1, fileLineCount);
  const int clampedEnd = std::clamp(endLine, clampedStart, fileLineCount);

  for (int line = clampedStart; line <= clampedEnd; ++line) {
    const std::string anchor =
        utils::Hashline::formatAnchor(line, buffer.lines[line - 1]);
    const std::string formatted =
        utils::Hashline::formatLine(line, buffer.lines[line - 1]);
    anchors.PushBack(rapidjson::Value(anchor.c_str(), alloc).Move(), alloc);
    lines.PushBack(rapidjson::Value(formatted.c_str(), alloc).Move(), alloc);
  }

  slice.AddMember("start_line", clampedStart, alloc);
  slice.AddMember("end_line", clampedEnd, alloc);
  slice.AddMember("lines", lines, alloc);
  slice.AddMember("anchors", anchors, alloc);
  return slice;
}

void addPostEditSlice(rapidjson::Document &doc, const FileBuffer &buffer,
                      int startLine, int endLine) {
  auto &alloc = doc.GetAllocator();
  doc.AddMember("post_edit_slice",
                buildHashlineSlice(buffer, startLine, endLine, alloc), alloc);
}

std::string describeEdit(const FileEditOperationInput &edit) {
  if ((edit.op == "replace_range" || edit.op == "delete_range") &&
      !edit.start_anchor.empty() && !edit.end_anchor.empty()) {
    return edit.op + " " + edit.start_anchor + " -> " + edit.end_anchor;
  }
  if ((edit.op == "insert_after" || edit.op == "insert_before") &&
      !edit.anchor.empty()) {
    return edit.op + " " + edit.anchor;
  }
  return edit.op.empty() ? "edit" : edit.op;
}

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
  const size_t pipePos = anchorText.find('|');
  if (pipePos != std::string::npos && pipePos + 1 < anchorText.size()) {
    throw std::runtime_error("Malformed anchor '" + anchorText +
                             "'. Use lineNumber#hash only, without trailing "
                             "|content from file_read.");
  }

  auto parsed = utils::Hashline::parseAnchor(anchorText);
  if (!parsed) {
    throw std::runtime_error("Malformed anchor '" + anchorText +
                             "'. Expected lineNumber#hash only.");
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
    throw std::runtime_error("Ambiguous relocated anchor '" + anchorText +
                             "': matched multiple nearby lines. Reread the "
                             "file and try again.");
  }

  throw std::runtime_error("Stale anchor '" + anchorText +
                           "': the file changed after your last read. Re-read "
                           "the file and retry with fresh anchors.");
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

void mergeSanitation(utils::HashlineTrimmer::SanitationResult *totals,
                     const utils::HashlineTrimmer::SanitationResult &delta) {
  if (!totals) {
    return;
  }
  totals->hashlinePrefixesStripped += delta.hashlinePrefixesStripped;
  totals->malformedHashFragmentsStripped +=
      delta.malformedHashFragmentsStripped;
  totals->diffMarkersStripped += delta.diffMarkersStripped;
  totals->boundaryEchoesRemoved += delta.boundaryEchoesRemoved;
  totals->boundaryEchoRemoved =
      totals->boundaryEchoRemoved || delta.boundaryEchoRemoved;
  totals->suspiciousContentFound =
      totals->suspiciousContentFound || delta.suspiciousContentFound;
  totals->suspiciousContentRejected =
      totals->suspiciousContentRejected || delta.suspiciousContentRejected;
}

std::vector<std::string>
sanitizeReplacementLines(const FileEditOperationInput &edit,
                         utils::HashlineTrimmer::SanitationResult *sanitation) {
  std::vector<std::string> sanitized;
  sanitized.reserve(edit.new_lines.size());

  for (size_t i = 0; i < edit.new_lines.size(); ++i) {
    utils::HashlineTrimmer::SanitationResult lineSanitation;
    std::string line = utils::HashlineTrimmer::sanitizeContent(
        edit.new_lines[i], &lineSanitation);
    if (utils::HashlineTrimmer::startsWithSuspiciousMetadata(line)) {
      lineSanitation.suspiciousContentFound = true;
      lineSanitation.suspiciousContentRejected = true;
      mergeSanitation(sanitation, lineSanitation);
      throw std::runtime_error(
          "Replacement text still appears to contain Hashline metadata. "
          "Remove lineNumber#hash| prefixes from new_lines.");
    }
    if (utils::HashlineTrimmer::startsWithSuspiciousDiffJunk(line)) {
      lineSanitation.suspiciousContentFound = true;
      lineSanitation.suspiciousContentRejected = true;
      mergeSanitation(sanitation, lineSanitation);
      throw std::runtime_error(
          "Replacement text still appears to contain diff markers. Remove "
          "leading + / - patch markers from new_lines.");
    }
    mergeSanitation(sanitation, lineSanitation);
    sanitized.push_back(std::move(line));
  }

  return sanitized;
}

bool hasMeaningfulHashlineEdits(const std::vector<FileEditOperationInput> &edits) {
  return std::any_of(edits.begin(), edits.end(), [](const auto &edit) {
    return !edit.op.empty() || !edit.start_anchor.empty() ||
           !edit.end_anchor.empty() || !edit.anchor.empty() ||
           !edit.new_lines.empty();
  });
}

bool hasMeaningfulLegacyReplace(const FileEditInput &input) {
  if (!input.has_old_string || !input.has_new_string) {
    return false;
  }

  return !input.old_string.empty() && !input.new_string.empty();
}

void stripBoundaryEchoes(std::vector<std::string> &newLines,
                         const std::vector<std::string> &lines, int startIndex,
                         int endIndexExclusive,
                         utils::HashlineTrimmer::SanitationResult *sanitation) {
  if (!newLines.empty() && startIndex > 0 &&
      newLines.front() == lines[startIndex - 1]) {
    newLines.erase(newLines.begin());
    if (sanitation) {
      sanitation->boundaryEchoRemoved = true;
      sanitation->boundaryEchoesRemoved++;
    }
  }

  if (!newLines.empty() &&
      endIndexExclusive < static_cast<int>(lines.size()) &&
      newLines.back() == lines[endIndexExclusive]) {
    newLines.pop_back();
    if (sanitation) {
      sanitation->boundaryEchoRemoved = true;
      sanitation->boundaryEchoesRemoved++;
    }
  }
}

void addStringArrayMember(rapidjson::Value &target, const char *name,
                          const std::vector<std::string> &values,
                          rapidjson::Document::AllocatorType &alloc) {
  rapidjson::Value array(rapidjson::kArrayType);
  for (const auto &value : values) {
    array.PushBack(rapidjson::Value(value.c_str(), alloc).Move(), alloc);
  }
  target.AddMember(rapidjson::Value(name, alloc).Move(), array, alloc);
}

void addSanitationMember(
    rapidjson::Value &target,
    const utils::HashlineTrimmer::SanitationResult &sanitation,
    rapidjson::Document::AllocatorType &alloc) {
  rapidjson::Value sanitationJson(rapidjson::kObjectType);
  sanitationJson.AddMember("hashline_prefixes_stripped",
                           sanitation.hashlinePrefixesStripped, alloc);
  sanitationJson.AddMember("malformed_hash_fragments_stripped",
                           sanitation.malformedHashFragmentsStripped, alloc);
  sanitationJson.AddMember("diff_markers_stripped",
                           sanitation.diffMarkersStripped, alloc);
  sanitationJson.AddMember("boundary_echoes_removed",
                           sanitation.boundaryEchoesRemoved, alloc);
  sanitationJson.AddMember("boundary_echo_removed",
                           sanitation.boundaryEchoRemoved, alloc);
  sanitationJson.AddMember("suspicious_content_found",
                           sanitation.suspiciousContentFound, alloc);
  sanitationJson.AddMember("suspicious_content_rejected",
                           sanitation.suspiciousContentRejected, alloc);
  target.AddMember("sanitation", sanitationJson, alloc);
}

rapidjson::Value buildOperationResult(
    const NormalizedEdit &edit, rapidjson::Document::AllocatorType &alloc) {
  rapidjson::Value op(rapidjson::kObjectType);
  const bool isInsertion = edit.startIndex == edit.endIndex;
  op.AddMember("op", rapidjson::Value(edit.op.c_str(), alloc).Move(), alloc);
  op.AddMember("description",
               rapidjson::Value(edit.description.c_str(), alloc).Move(), alloc);
  op.AddMember("start_line", edit.startIndex + 1, alloc);
  op.AddMember("end_line", isInsertion ? edit.startIndex + 1 : edit.endIndex,
               alloc);
  op.AddMember("new_line_count", static_cast<uint32_t>(edit.newLines.size()),
               alloc);
  op.AddMember("old_line_count", static_cast<uint32_t>(edit.oldLines.size()),
               alloc);
  op.AddMember("relocated", edit.relocated, alloc);
  addStringArrayMember(op, "old_lines", edit.oldLines, alloc);
  addStringArrayMember(op, "new_lines", edit.newLines, alloc);
  return op;
}

std::vector<NormalizedEdit>
normalizeEdits(const std::vector<FileEditOperationInput> &edits,
               const std::vector<std::string> &lines,
               utils::HashlineTrimmer::SanitationResult *sanitation = nullptr) {
  std::vector<NormalizedEdit> normalized;
  normalized.reserve(edits.size());

  for (const auto &edit : edits) {
    validateEditOperation(edit);
    FileEditOperationInput cleanEdit = edit;
    cleanEdit.new_lines = sanitizeReplacementLines(edit, sanitation);

    if (edit.op == "replace_range") {
      auto start = resolveAnchor(lines, edit.start_anchor);
      auto end = resolveAnchor(lines, edit.end_anchor);
      if (start.lineIndex > end.lineIndex) {
        throw std::runtime_error("Invalid replace_range: start anchor '" +
                                 edit.start_anchor +
                                 "' resolved after end anchor '" +
                                 edit.end_anchor + "'.");
      }
      std::vector<std::string> oldLines(lines.begin() + start.lineIndex,
                                        lines.begin() + end.lineIndex + 1);
      stripBoundaryEchoes(cleanEdit.new_lines, lines, start.lineIndex,
                          end.lineIndex + 1, sanitation);

      normalized.push_back({"replace_range",
                            start.lineIndex,
                            end.lineIndex + 1,
                            cleanEdit.new_lines,
                            "replace " + cleanEdit.start_anchor + "..." +
                                edit.end_anchor,
                            start.relocated || end.relocated,
                            oldLines});
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
      std::vector<std::string> oldLines(lines.begin() + start.lineIndex,
                                        lines.begin() + end.lineIndex + 1);
      normalized.push_back({"delete_range",
                            start.lineIndex,
                            end.lineIndex + 1,
                            {},
                            "delete " + edit.start_anchor + "..." +
                                edit.end_anchor,
                            start.relocated || end.relocated,
                            oldLines});
      continue;
    }

    if (edit.op == "insert_after") {
      auto anchor = resolveAnchor(lines, edit.anchor);
      normalized.push_back({"insert_after",
                            anchor.lineIndex + 1,
                            anchor.lineIndex + 1,
                            cleanEdit.new_lines,
                            "insert after " + cleanEdit.anchor,
                            anchor.relocated,
                            {}});
      continue;
    }

    if (edit.op == "insert_before") {
      auto anchor = resolveAnchor(lines, edit.anchor);
      normalized.push_back({"insert_before",
                            anchor.lineIndex,
                            anchor.lineIndex,
                            cleanEdit.new_lines,
                            "insert before " + cleanEdit.anchor,
                            anchor.relocated,
                            {}});
      continue;
    }

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

shared::ToolResult hashlineFailureResult(
    const FileEditInput &input, const std::vector<std::string> &lines,
    const std::string &message,
    const utils::HashlineTrimmer::SanitationResult &sanitation) {
  rapidjson::Document doc;
  doc.SetObject();
  auto &alloc = doc.GetAllocator();
  doc.AddMember("path", rapidjson::Value(input.path.c_str(), alloc).Move(),
                alloc);
  doc.AddMember("mode", rapidjson::Value("hashline_edits", alloc).Move(),
                alloc);
  doc.AddMember("error", rapidjson::Value(message.c_str(), alloc).Move(),
                alloc);

  rapidjson::Value operations(rapidjson::kArrayType);
  for (const auto &edit : input.edits) {
    rapidjson::Value op(rapidjson::kObjectType);
    op.AddMember("op", rapidjson::Value(edit.op.c_str(), alloc).Move(), alloc);
    const std::string description = describeEdit(edit);
    op.AddMember("description",
                 rapidjson::Value(description.c_str(), alloc).Move(), alloc);
    if (!edit.start_anchor.empty()) {
      op.AddMember("start_anchor",
                   rapidjson::Value(edit.start_anchor.c_str(), alloc).Move(),
                   alloc);
    }
    if (!edit.end_anchor.empty()) {
      op.AddMember("end_anchor",
                   rapidjson::Value(edit.end_anchor.c_str(), alloc).Move(),
                   alloc);
    }
    if (!edit.anchor.empty()) {
      op.AddMember("anchor", rapidjson::Value(edit.anchor.c_str(), alloc).Move(),
                   alloc);
    }
    op.AddMember("new_line_count",
                 static_cast<uint32_t>(edit.new_lines.size()), alloc);

    try {
      const auto normalized = normalizeEdits({edit}, lines, nullptr);
      if (!normalized.empty()) {
        op = buildOperationResult(normalized.front(), alloc);
      }
    } catch (const std::exception &e) {
      op.AddMember("error", rapidjson::Value(e.what(), alloc).Move(), alloc);
      addStringArrayMember(op, "old_lines", {}, alloc);
      try {
        utils::HashlineTrimmer::SanitationResult editSanitation;
        const auto sanitizedLines =
            sanitizeReplacementLines(edit, &editSanitation);
        addStringArrayMember(op, "new_lines", sanitizedLines, alloc);
      } catch (...) {
        addStringArrayMember(op, "new_lines", edit.new_lines, alloc);
      }
    }

    operations.PushBack(op, alloc);
  }

  doc.AddMember("operations", operations, alloc);
  addSanitationMember(doc, sanitation, alloc);

  rapidjson::StringBuffer sb;
  rapidjson::Writer<rapidjson::StringBuffer> writer(sb);
  doc.Accept(writer);
  return shared::ToolResult::fail(sb.GetString());
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
          "Edit files with Hashline anchors: read first, copy exact line#hash "
          "anchors, use small ops, and reread before the next edit call",
          ToolScope::FilesystemWrite};
}

std::shared_ptr<shared::JSONSchema> FileEditTool::getSchema() const {
  auto editSchema =
      zObject({{"op",
                zEnum({"replace_range", "insert_after", "insert_before",
                       "delete_range"})
                    ->describe("Hashline edit operation type. Use the "
                               "smallest op that matches the logical change "
                               "site.")},
               {"start_anchor",
                zString()
                    ->describe(
                        "Start anchor for range edits. Use ONLY the "
                        "lineNumber#hash anchor from file_read. Never include "
                        "the trailing |content. Copy the exact anchor from "
                        "file_read and do not adjust it within the same call.")
                    ->setOptional()},
               {"end_anchor",
                zString()
                    ->describe(
                        "End anchor for range edits. Use ONLY the "
                        "lineNumber#hash anchor from file_read. Never include "
                        "the trailing |content. Copy the exact anchor from "
                        "file_read and do not adjust it within the same call.")
                    ->setOptional()},
               {"anchor",
                zString()
                    ->describe(
                        "Single anchor for insert edits. Use ONLY the "
                        "lineNumber#hash anchor from file_read. Never include "
                        "the trailing |content. Prefer structural lines over "
                        "blank lines when choosing anchors.")
                    ->setOptional()},
               {"new_lines",
                zArray(zString())
                    ->describe("Plain replacement or inserted source lines "
                               "only, without trailing newline characters. "
                               "NEVER include Hashline prefixes, trailing "
                               "|content, diff markers, or unchanged boundary "
                               "echo lines from outside the replaced range.")
                    ->setOptional()}});

  return zObject(
             {{"path",
               zString()->describe("Absolute or relative path to the file")},
              {"edits",
               zArray(editSchema)
                   ->describe("Hashline operational manual for existing files: "
                              "1) read the file or relevant range with "
                              "file_read, 2) copy exact lineNumber#hash "
                              "anchors, 3) use the smallest op per logical "
                              "mutation site, 4) batch related edits for one "
                              "file into one call, 5) reread before the next "
                              "file_edit call on that file. ALL edits in one "
                              "call target the ORIGINAL file snapshot. Do NOT "
                              "adjust later anchors after earlier edits in "
                              "the same call. replace_range and delete_range "
                              "require BOTH start_anchor and end_anchor. "
                              "insert_before and insert_after require "
                              "anchor. Anchors must be lineNumber#hash only, "
                              "never lineNumber#hash|content. new_lines must "
                              "be plain source text only.")
                   ->setOptional()},
              {"content", zString()
                              ->describe("Whole-file content. Reserved for "
                                         "explicit new-file creation, not the "
                                         "normal path for editing an existing "
                                         "file when Hashline edits are "
                                         "available.")
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
  const bool hasAnchorEdits = hasMeaningfulHashlineEdits(input.edits);
  const bool hasOverwrite = input.has_content;
  const bool hasLegacyReplace = hasMeaningfulLegacyReplace(input);
  const int modeCount =
      static_cast<int>(hasAnchorEdits) + static_cast<int>(hasOverwrite) +
      static_cast<int>(hasLegacyReplace);

  if (modeCount > 1) {
    return shared::ToolResult::fail(
        "file_edit accepts exactly one editing mode per call. Use either "
        "Hashline edits, whole-file content for new-file creation, or legacy "
        "old_string/new_string compatibility mode.");
  }

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

      utils::HashlineTrimmer::SanitationResult sanitation;
      std::vector<NormalizedEdit> normalized;
      try {
        normalized = normalizeEdits(input.edits, buffer.lines, &sanitation);
      } catch (const std::exception &e) {
        return hashlineFailureResult(input, buffer.lines, e.what(), sanitation);
      }
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

      int sliceStart = static_cast<int>(buffer.lines.empty() ? 0 : buffer.lines.size());
      int sliceEnd = 1;

      rapidjson::Value operations(rapidjson::kArrayType);
      int cumulativeDelta = 0;
      for (const auto &edit : normalized) {
        rapidjson::Value op = buildOperationResult(edit, alloc);
        const int removedCount = edit.endIndex - edit.startIndex;
        const int finalStartIndex = edit.startIndex + cumulativeDelta;
        const int finalEndIndexExclusive =
            finalStartIndex + static_cast<int>(edit.newLines.size());
        const int localStartLine =
            std::max(1, finalStartIndex + 1 - kPostEditContextLines);
        const int localEndLine = edit.newLines.empty()
                                     ? finalStartIndex + kPostEditContextLines
                                     : finalEndIndexExclusive + kPostEditContextLines;
        op.AddMember(
            "post_edit_context",
            buildHashlineSlice(buffer, localStartLine, localEndLine, alloc),
            alloc);
        operations.PushBack(op, alloc);
        cumulativeDelta += static_cast<int>(edit.newLines.size()) - removedCount;

        const int insertedEndLine =
            edit.startIndex + static_cast<int>(edit.newLines.size());
        const int changedStartLine = edit.startIndex + 1;
        const int changedEndLine =
            std::max(changedStartLine,
                     std::max(edit.endIndex, insertedEndLine));
        sliceStart = std::min(sliceStart, changedStartLine);
        sliceEnd = std::max(sliceEnd, changedEndLine);
      }
      resDoc.AddMember("operations", operations, alloc);
      addSanitationMember(resDoc, sanitation, alloc);
      if (!buffer.lines.empty()) {
        addPostEditSlice(
            resDoc, buffer,
            std::max(1, sliceStart - kPostEditContextLines),
            std::min(static_cast<int>(buffer.lines.size()),
                     sliceEnd + kPostEditContextLines));
      } else {
        addPostEditSlice(resDoc, buffer, 0, 0);
      }
      return shared::ToolResult::ok(resDoc);
    }

    if (hasOverwrite) {
      if (fileExists) {
        return shared::ToolResult::fail(
            "Whole-file content overwrite is disabled for existing files. Use "
            "Hashline edits for modifications; content is reserved for explicit "
            "new-file creation.");
      }

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

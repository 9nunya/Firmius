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

constexpr int kAnchorSearchWindow = 15;

struct FileBuffer {
  std::vector<std::string> lines;
  bool hadTrailingNewline = false;
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
struct NormalizationError {
  size_t editIndex;
  utils::AnchorResult anchorResult;
  std::string errorMessage;
};

struct NormalizationResult {
  std::vector<NormalizedEdit> normalized;
  std::vector<NormalizationError> errors;
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
    return edit.op +
           " requires both start_anchor and end_anchor; got anchor only";
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

bool hasMeaningfulHashlineEdits(
    const std::vector<FileEditOperationInput> &edits) {
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

  if (!newLines.empty() && endIndexExclusive < static_cast<int>(lines.size()) &&
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

rapidjson::Value
buildOperationResult(const NormalizedEdit &edit,
                     rapidjson::Document::AllocatorType &alloc) {
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

std::string buildDiffPreview(const std::vector<NormalizedEdit> &normalized) {
  std::ostringstream out;
  for (std::size_t i = 0; i < normalized.size(); ++i) {
    const auto &edit = normalized[i];
    if (i > 0) {
      out << "\n";
    }
    out << "@@ " << edit.description << " @@\n";
    for (const auto &line : edit.oldLines) {
      out << "-" << line << "\n";
    }
    for (const auto &line : edit.newLines) {
      out << "+" << line << "\n";
    }
  }
  return out.str();
}

NormalizationResult
normalizeEdits(const std::vector<FileEditOperationInput> &edits,
               const std::vector<std::string> &lines,
               utils::HashlineTrimmer::SanitationResult *sanitation = nullptr) {
  NormalizationResult result;

  for (size_t i = 0; i < edits.size(); ++i) {
    const auto &edit = edits[i];
    try {
      validateEditOperation(edit);
    } catch (const std::exception &e) {
      result.errors.push_back({i, {}, e.what()});
      continue;
    }
    FileEditOperationInput cleanEdit = edit;
    try {
      cleanEdit.new_lines = sanitizeReplacementLines(edit, sanitation);
    } catch (const std::exception &e) {
      result.errors.push_back({i, {}, e.what()});
      continue;
    }

    if (edit.op == "replace_range") {
      auto start = utils::Hashline::resolveAnchor(lines, edit.start_anchor,
                                                  kAnchorSearchWindow);
      auto end = utils::Hashline::resolveAnchor(lines, edit.end_anchor,
                                                kAnchorSearchWindow);
      if (start.status != utils::AnchorResult::Status::SUCCESS) {
        result.errors.push_back({i, start, start.errorMessage});
      }
      if (end.status != utils::AnchorResult::Status::SUCCESS) {
        result.errors.push_back({i, end, end.errorMessage});
      }
      if (start.status == utils::AnchorResult::Status::SUCCESS &&
          end.status == utils::AnchorResult::Status::SUCCESS) {
        if (start.lineIndex > end.lineIndex) {
          result.errors.push_back(
              {i,
               {},
               "Invalid replace_range: start anchor '" + edit.start_anchor +
                   "' resolved after end anchor '" + edit.end_anchor + "'."});
          continue;
        }
        std::vector<std::string> oldLines(lines.begin() + start.lineIndex,
                                          lines.begin() + end.lineIndex + 1);
        stripBoundaryEchoes(cleanEdit.new_lines, lines, start.lineIndex,
                            end.lineIndex + 1, sanitation);
        result.normalized.push_back(
            {"replace_range", start.lineIndex, end.lineIndex + 1,
             cleanEdit.new_lines,
             "replace " + cleanEdit.start_anchor + "..." + edit.end_anchor,
             start.relocated || end.relocated, oldLines});
      }
    } else if (edit.op == "delete_range") {
      auto start = utils::Hashline::resolveAnchor(lines, edit.start_anchor,
                                                  kAnchorSearchWindow);
      auto end = utils::Hashline::resolveAnchor(lines, edit.end_anchor,
                                                kAnchorSearchWindow);
      if (start.status != utils::AnchorResult::Status::SUCCESS) {
        result.errors.push_back({i, start, start.errorMessage});
      }
      if (end.status != utils::AnchorResult::Status::SUCCESS) {
        result.errors.push_back({i, end, end.errorMessage});
      }
      if (start.status == utils::AnchorResult::Status::SUCCESS &&
          end.status == utils::AnchorResult::Status::SUCCESS) {
        if (start.lineIndex > end.lineIndex) {
          result.errors.push_back(
              {i,
               {},
               "Invalid delete_range: start anchor '" + edit.start_anchor +
                   "' resolved after end anchor '" + edit.end_anchor + "'."});
          continue;
        }
        std::vector<std::string> oldLines(lines.begin() + start.lineIndex,
                                          lines.begin() + end.lineIndex + 1);
        result.normalized.push_back(
            {"delete_range",
             start.lineIndex,
             end.lineIndex + 1,
             {},
             "delete " + edit.start_anchor + "..." + edit.end_anchor,
             start.relocated || end.relocated,
             oldLines});
      }
    } else if (edit.op == "insert_after") {
      auto anchor = utils::Hashline::resolveAnchor(lines, edit.anchor,
                                                   kAnchorSearchWindow);
      if (anchor.status != utils::AnchorResult::Status::SUCCESS) {
        result.errors.push_back({i, anchor, anchor.errorMessage});
      } else {
        result.normalized.push_back({"insert_after",
                                     anchor.lineIndex + 1,
                                     anchor.lineIndex + 1,
                                     cleanEdit.new_lines,
                                     "insert after " + cleanEdit.anchor,
                                     anchor.relocated,
                                     {}});
      }
    } else if (edit.op == "insert_before") {
      auto anchor = utils::Hashline::resolveAnchor(lines, edit.anchor,
                                                   kAnchorSearchWindow);
      if (anchor.status != utils::AnchorResult::Status::SUCCESS) {
        result.errors.push_back({i, anchor, anchor.errorMessage});
      } else {
        result.normalized.push_back({"insert_before",
                                     anchor.lineIndex,
                                     anchor.lineIndex,
                                     cleanEdit.new_lines,
                                     "insert before " + cleanEdit.anchor,
                                     anchor.relocated,
                                     {}});
      }
    }
  }

  if (!result.errors.empty()) {
    return result;
  }

  std::stable_sort(result.normalized.begin(), result.normalized.end(),
                   [](const NormalizedEdit &left, const NormalizedEdit &right) {
                     if (left.startIndex != right.startIndex)
                       return left.startIndex < right.startIndex;
                     return left.endIndex < right.endIndex;
                   });

  for (size_t i = 1; i < result.normalized.size(); ++i) {
    if (editsConflict(result.normalized[i - 1], result.normalized[i])) {
      result.errors.push_back({static_cast<size_t>(-1),
                               {},
                               "Overlapping edits are not allowed: '" +
                                   result.normalized[i - 1].description +
                                   "' conflicts with '" +
                                   result.normalized[i].description + "'."});
    }
  }

  return result;
}

shared::ToolResult hashlineFailureResult(
    const FileEditInput &input, const NormalizationResult &normResult,
    const utils::HashlineTrimmer::SanitationResult &sanitation) {
  rapidjson::Document doc;
  doc.SetObject();
  auto &alloc = doc.GetAllocator();
  doc.AddMember("path", rapidjson::Value(input.path.c_str(), alloc).Move(),
                alloc);
  doc.AddMember("mode", rapidjson::Value("hashline_edits", alloc).Move(),
                alloc);
  doc.AddMember(
      "error",
      rapidjson::Value(
          "Batch edit failed. One or more edits could not be applied.", alloc)
          .Move(),
      alloc);

  rapidjson::Value batchErrors(rapidjson::kArrayType);
  for (const auto &err : normResult.errors) {
    rapidjson::Value errorObj(rapidjson::kObjectType);
    if (err.editIndex != static_cast<size_t>(-1)) {
      errorObj.AddMember("edit_index", static_cast<uint32_t>(err.editIndex),
                         alloc);
      errorObj.AddMember(
          "operation",
          rapidjson::Value(input.edits[err.editIndex].op.c_str(), alloc).Move(),
          alloc);
    }
    errorObj.AddMember("message",
                       rapidjson::Value(err.errorMessage.c_str(), alloc).Move(),
                       alloc);
    if (!err.anchorResult.expectedHash.empty()) {
      errorObj.AddMember(
          "expected_hash",
          rapidjson::Value(err.anchorResult.expectedHash.c_str(), alloc).Move(),
          alloc);
    }
    if (!err.anchorResult.foundHash.empty()) {
      errorObj.AddMember(
          "found_hash",
          rapidjson::Value(err.anchorResult.foundHash.c_str(), alloc).Move(),
          alloc);
    }
    batchErrors.PushBack(errorObj, alloc);
  }
  doc.AddMember("batch_errors", batchErrors, alloc);

  rapidjson::Value operations(rapidjson::kArrayType);
  for (size_t i = 0; i < input.edits.size(); ++i) {
    const auto &edit = input.edits[i];
    rapidjson::Value op(rapidjson::kObjectType);
    op.AddMember("op", rapidjson::Value(edit.op.c_str(), alloc).Move(), alloc);
    op.AddMember("description",
                 rapidjson::Value(describeEdit(edit).c_str(), alloc).Move(),
                 alloc);

    bool found = false;
    for (const auto &norm : normResult.normalized) {
      if (norm.description.find(edit.op) !=
          std::string::npos) { // Very rough match
        op = buildOperationResult(norm, alloc);
        found = true;
        break;
      }
    }
    if (!found) {
      for (const auto &err : normResult.errors) {
        if (err.editIndex == i) {
          op.AddMember("error",
                       rapidjson::Value(err.errorMessage.c_str(), alloc).Move(),
                       alloc);
          break;
        }
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
    matchIndices =
        StringUtil::findFuzzy(content, input.old_string, input.fuzzy_threshold);
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
  ctx.agent.getEnvironment()->getWorkspace().recordFileEdit(absolutePath);

  rapidjson::Document resDoc;
  resDoc.SetObject();
  resDoc.AddMember(
      "path",
      rapidjson::Value(input.path.c_str(), resDoc.GetAllocator()).Move(),
      resDoc.GetAllocator());
  resDoc.AddMember(
      "mode",
      rapidjson::Value("legacy_string_replace", resDoc.GetAllocator()).Move(),
      resDoc.GetAllocator());
  resDoc.AddMember("occurrences", static_cast<uint32_t>(occurrences),
                   resDoc.GetAllocator());
  return shared::ToolResult::ok(resDoc);
}

bool isAnchorRead(const std::string &absolutePath, const std::string &anchor,
                  const IWorkspace &workspace) {
  if (anchor.empty()) {
    return true;
  }
  size_t hashPos = anchor.find('#');
  if (hashPos == std::string::npos) {
    return false;
  }
  try {
    int line = std::stoi(anchor.substr(0, hashPos));
    return workspace.isLineRead(absolutePath, line);
  } catch (...) {
    return false;
  }
}

} // namespace

shared::ToolMetadata FileEditTool::getMetadata() const {
  return {"file_edit",
          "Edit files with Hashline anchors: read first, copy exact line#hash "
          "anchors, use small ops, and reread before the next edit call",
          ToolScope::FilesystemWrite};
}

std::shared_ptr<shared::JSONSchema> FileEditTool::getSchema() const {
  auto editSchema = zObject(
      {{"op", zEnum({"replace_range", "insert_after", "insert_before",
                     "delete_range"})
                  ->describe("Hashline edit operation type. Use the "
                             "smallest op that matches the logical change "
                             "site.")},
       {"start_anchor",
        zString()
            ->describe("Start anchor for range edits. Use ONLY the "
                       "lineNumber#hash anchor from file_read. Never include "
                       "the trailing |content. Copy the exact anchor from "
                       "file_read and do not adjust it within the same call.")
            ->setOptional()},
       {"end_anchor",
        zString()
            ->describe("End anchor for range edits. Use ONLY the "
                       "lineNumber#hash anchor from file_read. Never include "
                       "the trailing |content. Copy the exact anchor from "
                       "file_read and do not adjust it within the same call.")
            ->setOptional()},
       {"anchor",
        zString()
            ->describe("Single anchor for insert edits. Use ONLY the "
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
  const int modeCount = static_cast<int>(hasAnchorEdits) +
                        static_cast<int>(hasOverwrite) +
                        static_cast<int>(hasLegacyReplace);

  if (modeCount > 1) {
    return shared::ToolResult::fail(
        "file_edit accepts exactly one editing mode per call. Use either "
        "Hashline edits, whole-file content for new-file creation, or legacy "
        "old_string/new_string compatibility mode.");
  }

  if (fileExists) {
    auto &workspace = ctx.agent.getEnvironment()->getWorkspace();
    if (!workspace.hasFullyReadFile(absolutePath)) {
      if (hasAnchorEdits) {
        for (const auto &edit : input.edits) {
          if (!edit.start_anchor.empty() &&
              !isAnchorRead(absolutePath, edit.start_anchor, workspace)) {
            return shared::ToolResult::fail(
                "Anchor '" + edit.start_anchor +
                "' has not been read. You must read the lines you intend to "
                "edit. Use 'file_read' on '" +
                input.path + "' to refresh your context.");
          }
          if (!edit.end_anchor.empty() &&
              !isAnchorRead(absolutePath, edit.end_anchor, workspace)) {
            return shared::ToolResult::fail(
                "Anchor '" + edit.end_anchor +
                "' has not been read. You must read the lines you intend to "
                "edit. Use 'file_read' on '" +
                input.path + "' to refresh your context.");
          }
          if (!edit.anchor.empty() &&
              !isAnchorRead(absolutePath, edit.anchor, workspace)) {
            return shared::ToolResult::fail(
                "Anchor '" + edit.anchor +
                "' has not been read. You must read the lines you intend to "
                "edit. Use 'file_read' on '" +
                input.path + "' to refresh your context.");
          }
        }
      } else {
        return shared::ToolResult::fail(
            "You MUST READ the ENTIRE file before editing it. Use 'file_read' "
            "on '" +
            input.path +
            "' first, then reference the returned Hashline anchors in "
            "file_edit.");
      }
    }
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
      NormalizationResult normResult =
          normalizeEdits(input.edits, buffer.lines, &sanitation);
      if (!normResult.errors.empty()) {
        return hashlineFailureResult(input, normResult, sanitation);
      }
      normalized = normResult.normalized;
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
      ctx.agent.getEnvironment()->getWorkspace().recordFileEdit(absolutePath);

      rapidjson::Document resDoc;
      resDoc.SetObject();
      auto &alloc = resDoc.GetAllocator();
      resDoc.AddMember(
          "path", rapidjson::Value(input.path.c_str(), alloc).Move(), alloc);
      resDoc.AddMember("mode", rapidjson::Value("hashline_edits", alloc).Move(),
                       alloc);
      resDoc.AddMember("applied_edits",
                       static_cast<uint32_t>(normalized.size()), alloc);
      resDoc.AddMember("removed_lines", removedLines, alloc);
      resDoc.AddMember("added_lines", addedLines, alloc);
      resDoc.AddMember("relocated_anchors", relocatedAnchors, alloc);
      const std::string diffPreview = buildDiffPreview(normalized);
      resDoc.AddMember("diff_preview",
                       rapidjson::Value(diffPreview.c_str(), alloc).Move(),
                       alloc);
      resDoc.AddMember("watch_state",
                       rapidjson::Value("refreshed", alloc).Move(), alloc);

      rapidjson::Value operations(rapidjson::kArrayType);
      for (const auto &edit : normalized) {
        rapidjson::Value op = buildOperationResult(edit, alloc);
        operations.PushBack(op, alloc);
      }
      resDoc.AddMember("operations", operations, alloc);
      addSanitationMember(resDoc, sanitation, alloc);
      return shared::ToolResult::ok(resDoc);
    }

    if (hasOverwrite) {
      if (fileExists) {
        return shared::ToolResult::fail(
            "Whole-file content overwrite is disabled for existing files. Use "
            "Hashline edits for modifications; content is reserved for "
            "explicit "
            "new-file creation.");
      }

      ctx.host.writeFile(
          absolutePath,
          std::vector<uint8_t>(input.content.begin(), input.content.end()));
      ctx.agent.getEnvironment()->getWorkspace().recordFileEdit(absolutePath);

      rapidjson::Document resDoc;
      resDoc.SetObject();
      auto &alloc = resDoc.GetAllocator();
      resDoc.AddMember(
          "path", rapidjson::Value(input.path.c_str(), alloc).Move(), alloc);
      resDoc.AddMember("mode", rapidjson::Value("overwrite", alloc).Move(),
                       alloc);
      resDoc.AddMember("bytes_written",
                       static_cast<uint32_t>(input.content.size()), alloc);
      resDoc.AddMember("watch_state",
                       rapidjson::Value("refreshed", alloc).Move(), alloc);
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

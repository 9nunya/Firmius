#include "tools/FileEditTool.hpp"
#include "agents/Agent.hpp"
#include "lsp/LspService.hpp"
#include "utils/LineRange.hpp"
#include "utils/StringUtil.hpp"
#include <algorithm>
#include <optional>
#include <rapidjson/document.h>
#include <regex>
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

struct FileEditExecutionResult {
  bool success = false;
  rapidjson::Document doc;
  std::string failureMessage;
};

enum class StructuredEditMode {
  None,
  LineRange,
  SearchReplace,
  Mixed,
};

rapidjson::Value cloneJsonValue(const rapidjson::Value &value,
                                rapidjson::Document::AllocatorType &alloc) {
  rapidjson::Value clone;
  clone.CopyFrom(value, alloc);
  return clone;
}

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
  throw std::runtime_error(edit.op + " requires anchor");
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

  if (edit.op == "search_replace") {
    if (!edit.has_old_string) {
      throw std::runtime_error("search_replace requires old_string");
    }
    if (!edit.has_new_string) {
      throw std::runtime_error("search_replace requires new_string");
    }
    if (edit.old_string.empty()) {
      throw std::runtime_error("search_replace old_string cannot be empty");
    }
    return;
  }

  throw std::runtime_error("Malformed edit op '" + edit.op +
                           "'. Expected replace_range, insert_after, "
                           "insert_before, delete_range, or search_replace.");
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
  if (edit.op == "search_replace" && edit.has_old_string) {
    return "search_replace " + edit.old_string;
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

std::optional<NormalizedEdit>
buildSearchReplacePreview(const std::string &before,
                          const std::string &after,
                          const FileEditOperationInput &edit) {
  if (before == after) {
    return std::nullopt;
  }

  const auto beforeBuffer = splitFileContent(before);
  const auto afterBuffer = splitFileContent(after);
  const auto &beforeLines = beforeBuffer.lines;
  const auto &afterLines = afterBuffer.lines;

  size_t prefix = 0;
  while (prefix < beforeLines.size() && prefix < afterLines.size() &&
         beforeLines[prefix] == afterLines[prefix]) {
    ++prefix;
  }

  size_t beforeSuffix = beforeLines.size();
  size_t afterSuffix = afterLines.size();
  while (beforeSuffix > prefix && afterSuffix > prefix &&
         beforeLines[beforeSuffix - 1] == afterLines[afterSuffix - 1]) {
    --beforeSuffix;
    --afterSuffix;
  }

  NormalizedEdit normalized;
  normalized.op = edit.op;
  normalized.description = describeEdit(edit);
  normalized.startIndex = static_cast<int>(prefix);
  normalized.endIndex = static_cast<int>(beforeSuffix);
  normalized.oldLines.assign(beforeLines.begin() + static_cast<long>(prefix),
                             beforeLines.begin() +
                                 static_cast<long>(beforeSuffix));
  normalized.newLines.assign(afterLines.begin() + static_cast<long>(prefix),
                             afterLines.begin() +
                                 static_cast<long>(afterSuffix));
  return normalized;
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

void mergeSanitation(utils::LineRangeTrimmer::SanitationResult *totals,
                     const utils::LineRangeTrimmer::SanitationResult &delta) {
  if (!totals) {
    return;
  }
  totals->lineRangePrefixesStripped += delta.lineRangePrefixesStripped;
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
                         utils::LineRangeTrimmer::SanitationResult *sanitation) {
  std::vector<std::string> sanitized;
  sanitized.reserve(edit.new_lines.size());

  for (size_t i = 0; i < edit.new_lines.size(); ++i) {
    utils::LineRangeTrimmer::SanitationResult lineSanitation;
    std::string line = utils::LineRangeTrimmer::sanitizeContent(
        edit.new_lines[i], &lineSanitation);
    if (utils::LineRangeTrimmer::startsWithSuspiciousMetadata(line)) {
      lineSanitation.suspiciousContentFound = true;
      lineSanitation.suspiciousContentRejected = true;
      mergeSanitation(sanitation, lineSanitation);
      throw std::runtime_error(
          "Replacement text still appears to contain Hashline metadata. "
          "Remove lineNumber#hash| prefixes or trailing hash fragments from "
          "new_lines.");
    }
    if (utils::LineRangeTrimmer::startsWithSuspiciousDiffJunk(line)) {
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

bool hasMeaningfulEditInput(const FileEditOperationInput &edit) {
  if (!edit.op.empty() || !edit.start_anchor.empty() ||
      !edit.end_anchor.empty() || !edit.anchor.empty() ||
      !edit.new_lines.empty()) {
    return true;
  }

  if (!edit.old_string.empty()) {
    return true;
  }

  if (edit.has_new_string && (edit.has_old_string || !edit.old_string.empty())) {
    return true;
  }

  if (edit.replace_all &&
      (edit.has_old_string || edit.has_new_string || !edit.old_string.empty() ||
       !edit.new_string.empty())) {
    return true;
  }

  return false;
}

void sanitizeEditOperation(FileEditOperationInput &edit) {
  if (edit.op.empty() && !edit.old_string.empty() &&
      (edit.has_new_string || !edit.new_string.empty())) {
    edit.op = "search_replace";
  }
  if (edit.op != "search_replace") {
    if (edit.has_old_string && edit.old_string.empty()) {
      edit.has_old_string = false;
    }
    if (edit.has_new_string && edit.new_string.empty()) {
      edit.has_new_string = false;
    }
  }
}

void sanitizeTargetInput(FileEditTargetInput &input) {
  for (auto &edit : input.edits) {
    sanitizeEditOperation(edit);
  }
  input.edits.erase(
      std::remove_if(input.edits.begin(), input.edits.end(),
                     [](const FileEditOperationInput &edit) {
                       return !hasMeaningfulEditInput(edit);
                     }),
      input.edits.end());
}

bool isSearchReplaceEdit(const FileEditOperationInput &edit) {
  return edit.op == "search_replace" || !edit.old_string.empty() ||
         (!edit.new_string.empty() && edit.has_new_string);
}

StructuredEditMode
classifyStructuredEditMode(const std::vector<FileEditOperationInput> &edits) {
  bool sawLineRange = false;
  bool sawSearchReplace = false;
  for (const auto &edit : edits) {
    if (!hasMeaningfulEditInput(edit)) {
      continue;
    }
    if (isSearchReplaceEdit(edit)) {
      sawSearchReplace = true;
    } else {
      sawLineRange = true;
    }
  }

  if (sawLineRange && sawSearchReplace) {
    return StructuredEditMode::Mixed;
  }
  if (sawLineRange) {
    return StructuredEditMode::LineRange;
  }
  if (sawSearchReplace) {
    return StructuredEditMode::SearchReplace;
  }
  return StructuredEditMode::None;
}

class PatchParser {
public:
  struct Hunk {
    int startPatchLine;
    int oldStartLine = 0;
    int oldCount = 0;
    int newStartLine = 0;
    int newCount = 0;
    std::string startAnchor;
    std::string endAnchor;
    std::vector<std::string> contextLines;
    std::vector<std::string> oldLines;
    std::vector<std::string> newLines;
  };

  static std::vector<FileEditOperationInput>
  parse(const std::string &patchText) {
    std::vector<FileEditOperationInput> edits;
    std::istringstream stream(patchText);
    std::string line;
    int lineNo = 0;

    std::vector<Hunk> hunks;
    static const std::regex githubPattern(
        R"(^@@\s+-(\d+)(?:,(\d+))?\s+\+(\d+)(?:,(\d+))?\s+@@.*)");
    static const std::regex anchorPattern(
        R"(^@@\s+([^\s]+?)(?:\.\.\.([^\s]+?))?\s+@@$)");

    Hunk *currentHunk = nullptr;
    bool inHunk = false;
    bool pastChanges = false;

    while (std::getline(stream, line)) {
      lineNo++;
      if (line.empty() && !inHunk) continue;
      if (line.rfind("--- ", 0) == 0 || line.rfind("+++ ", 0) == 0) continue;

      std::smatch match;
      if (std::regex_match(line, match, githubPattern)) {
        hunks.push_back({});
        currentHunk = &hunks.back();
        currentHunk->startPatchLine = lineNo;
        currentHunk->oldStartLine = std::stoi(match[1].str());
        currentHunk->oldCount = match[2].matched ? std::stoi(match[2].str()) : 1;
        currentHunk->newStartLine = std::stoi(match[3].str());
        currentHunk->newCount = match[4].matched ? std::stoi(match[4].str()) : 1;
        inHunk = true;
        pastChanges = false;
        continue;
      }

      if (std::regex_match(line, match, anchorPattern)) {
        hunks.push_back({});
        currentHunk = &hunks.back();
        currentHunk->startPatchLine = lineNo;
        currentHunk->startAnchor = match[1].str();
        currentHunk->endAnchor = match[2].matched ? match[2].str() : "";
        inHunk = true;
        pastChanges = false;
        continue;
      }

      if (!inHunk) {
        throw std::runtime_error(
            "Malformed patch at line " + std::to_string(lineNo) +
            ": expected '@@ -n,m +n,m @@' or '@@ anchor @@'");
      }

      if (line.empty()) { currentHunk->newLines.push_back(""); continue; }

      char prefix = line[0];
      std::string content = line.substr(1);

      if (prefix == '-') {
        if (pastChanges && !currentHunk->newLines.empty()) {
          throw std::runtime_error("Malformed patch at line " +
              std::to_string(lineNo) + ": '-' after '+' in hunk");
        }
        currentHunk->oldLines.push_back(content);
      } else if (prefix == '+') {
        currentHunk->newLines.push_back(content);
        pastChanges = true;
      } else if (prefix == ' ') {
        if (!pastChanges && currentHunk->oldLines.empty()) {
          currentHunk->contextLines.push_back(content);
        }
      } else if (prefix == '\\') {
        continue;
      }
    }

    for (const auto &hunk : hunks) {
      FileEditOperationInput edit;
      edit.patch_line = hunk.startPatchLine;

      if (hunk.oldLines.empty() && hunk.newLines.empty()) {
        throw std::runtime_error("Malformed patch: hunk at line " +
            std::to_string(hunk.startPatchLine) + " has no changes");
      }

      if (!hunk.startAnchor.empty()) {
        if (hunk.oldLines.empty()) {
          if (!hunk.endAnchor.empty()) {
            throw std::runtime_error("Malformed patch: range anchor with only '+' lines");
          }
          edit.op = "insert_after";
          edit.anchor = hunk.startAnchor;
        } else if (hunk.newLines.empty()) {
          edit.op = "delete_range";
          edit.start_anchor = hunk.startAnchor;
          edit.end_anchor = hunk.endAnchor.empty() ? hunk.startAnchor : hunk.endAnchor;
        } else {
          edit.op = "replace_range";
          edit.start_anchor = hunk.startAnchor;
          edit.end_anchor = hunk.endAnchor.empty() ? hunk.startAnchor : hunk.endAnchor;
        }
        edit.new_lines = hunk.newLines;
      } else {
        int startLine = hunk.oldStartLine + static_cast<int>(hunk.contextLines.size());
        int endLine = startLine + static_cast<int>(hunk.oldLines.size()) - 1;

        if (hunk.oldLines.empty()) {
          int insertAfter = hunk.oldStartLine + static_cast<int>(hunk.contextLines.size()) - 1;
          if (hunk.contextLines.empty()) {
            edit.op = "insert_before";
            edit.anchor = "1";
          } else {
            edit.op = "insert_after";
            edit.anchor = std::to_string(insertAfter);
          }
        } else if (hunk.newLines.empty()) {
          edit.op = "delete_range";
          edit.start_anchor = std::to_string(startLine);
          edit.end_anchor = std::to_string(endLine);
        } else {
          edit.op = "replace_range";
          edit.start_anchor = std::to_string(startLine);
          edit.end_anchor = std::to_string(endLine);
        }
        edit.new_lines = hunk.newLines;
      }
      edits.push_back(std::move(edit));
    }
    return edits;
  }
};


bool hasMeaningfulLegacyReplace(const FileEditTargetInput &input) {
  if (!input.has_old_string || !input.has_new_string) {
    return false;
  }

  return !input.old_string.empty() && !input.new_string.empty();
}

bool hasMeaningfulLegacyPayload(const FileEditTargetInput &input) {
  return !input.old_string.empty() || !input.new_string.empty();
}

bool hasMeaningfulPatch(const FileEditTargetInput &input) {
  return input.has_patch && !input.patch.empty();
}

bool hasContentOnlyEnvelope(const FileEditTargetInput &input,
                            bool hasStructuredEdits, bool hasLegacyReplace,
                            bool hasPatch) {
  if (!input.has_content) {
    return false;
  }

  return !hasStructuredEdits && !hasLegacyReplace && !hasPatch;
}

bool hasMeaningfulOverwrite(const FileEditTargetInput &input,
                            bool hasStructuredEdits, bool hasLegacyReplace,
                            bool hasPatch) {
  return hasContentOnlyEnvelope(input, hasStructuredEdits, hasLegacyReplace,
                                hasPatch) &&
         !input.content.empty();
}

bool hasMeaningfulTopLevelPayloadForMultiFile(const FileEditInput &input) {
  if (!input.path.empty()) {
    return true;
  }

  if (input.has_content && !input.content.empty()) {
    return true;
  }

  if (hasMeaningfulPatch(input)) {
    return true;
  }

  if (hasMeaningfulLegacyPayload(input)) {
    return true;
  }

  return std::any_of(input.edits.begin(), input.edits.end(),
                     [](const FileEditOperationInput &edit) {
                       return hasMeaningfulEditInput(edit);
                     });
}

bool hasMeaningfulTargetEnvelope(const FileEditTargetInput &input) {
  if (!input.path.empty()) {
    return true;
  }

  if (input.has_content && !input.content.empty()) {
    return true;
  }

  if (hasMeaningfulPatch(input) || hasMeaningfulLegacyPayload(input)) {
    return true;
  }

  return std::any_of(input.edits.begin(), input.edits.end(),
                     [](const FileEditOperationInput &edit) {
                       return hasMeaningfulEditInput(edit);
                     });
}

std::vector<std::string>
collectMeaningfulTopLevelFieldsForMultiFile(const FileEditInput &input) {
  std::vector<std::string> fields;

  if (!input.path.empty()) {
    fields.emplace_back("path");
  }
  if (input.has_content && !input.content.empty()) {
    fields.emplace_back("content");
  }
  if (hasMeaningfulPatch(input)) {
    fields.emplace_back("patch");
  }
  if (std::any_of(input.edits.begin(), input.edits.end(),
                  [](const FileEditOperationInput &edit) {
                    return hasMeaningfulEditInput(edit);
                  })) {
    fields.emplace_back("edits");
  }
  if (hasMeaningfulLegacyPayload(input)) {
    fields.emplace_back("old_string/new_string");
  }

  return fields;
}

std::string buildMultiFileMixedModeError(const FileEditInput &input) {
  const auto fields = collectMeaningfulTopLevelFieldsForMultiFile(input);
  std::string message =
      "file_edit files[] mode ignores semantically inert wrapper defaults such "
      "as content:\"\", patch:\"\", edits:[], old_string:\"\", new_string:\"\", "
      "replace_all:false, and fuzzy_threshold:0. ";

  if (!fields.empty()) {
    message += "This request still mixes files[] with meaningful top-level ";
    message += joinStrings(fields, ", ");
    message +=
        ". Move those fields into each files[] entry or remove them from the "
        "top level.";
  } else {
    message +=
        "Use files[] by itself, or use a single top-level target without "
        "files[].";
  }

  return message;
}

void stripBoundaryEchoes(std::vector<std::string> &newLines,
                         const std::vector<std::string> &lines, int startIndex,
                         int endIndexExclusive,
                         utils::LineRangeTrimmer::SanitationResult *sanitation) {
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
    const utils::LineRangeTrimmer::SanitationResult &sanitation,
    rapidjson::Document::AllocatorType &alloc) {
  rapidjson::Value sanitationJson(rapidjson::kObjectType);
  sanitationJson.AddMember("linerange_prefixes_stripped",
                           sanitation.lineRangePrefixesStripped, alloc);
  sanitationJson.AddMember("line_range_prefixes_stripped",
                           sanitation.lineRangePrefixesStripped, alloc);
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
               utils::LineRangeTrimmer::SanitationResult *sanitation = nullptr) {
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
      auto start = utils::LineRange::resolveAnchor(lines, edit.start_anchor);
      auto end = utils::LineRange::resolveAnchor(lines, edit.end_anchor);
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
               "Invalid replace_range: start line " + edit.start_anchor +
                   " is after end line " + edit.end_anchor + "."});
          continue;
        }
        std::vector<std::string> oldLines(lines.begin() + start.lineIndex,
                                          lines.begin() + end.lineIndex + 1);
        stripBoundaryEchoes(cleanEdit.new_lines, lines, start.lineIndex,
                            end.lineIndex + 1, sanitation);
        result.normalized.push_back(
            {"replace_range", start.lineIndex, end.lineIndex + 1,
             cleanEdit.new_lines,
             "replace " + edit.start_anchor + "..." + edit.end_anchor,
             start.relocated || end.relocated, oldLines});
      }
    } else if (edit.op == "delete_range") {
      auto start = utils::LineRange::resolveAnchor(lines, edit.start_anchor);
      auto end = utils::LineRange::resolveAnchor(lines, edit.end_anchor);
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
               "Invalid delete_range: start line " + edit.start_anchor +
                   " is after end line " + edit.end_anchor + "."});
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
      auto anchor = utils::LineRange::resolveAnchor(lines, edit.anchor);
      if (anchor.status != utils::AnchorResult::Status::SUCCESS) {
        result.errors.push_back({i, anchor, anchor.errorMessage});
      } else {
        result.normalized.push_back({"insert_after",
                                     anchor.lineIndex + 1,
                                     anchor.lineIndex + 1,
                                     cleanEdit.new_lines,
                                     "insert after " + edit.anchor,
                                     anchor.relocated,
                                     {}});
      }
    } else if (edit.op == "insert_before") {
      auto anchor = utils::LineRange::resolveAnchor(lines, edit.anchor);
      if (anchor.status != utils::AnchorResult::Status::SUCCESS) {
        result.errors.push_back({i, anchor, anchor.errorMessage});
      } else {
        result.normalized.push_back({"insert_before",
                                     anchor.lineIndex,
                                     anchor.lineIndex,
                                     cleanEdit.new_lines,
                                     "insert before " + edit.anchor,
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

rapidjson::Document buildLineRangeFailureDoc(
    const FileEditTargetInput &input, const NormalizationResult &normResult,
    const utils::LineRangeTrimmer::SanitationResult &sanitation) {
  rapidjson::Document doc;
  doc.SetObject();
  auto &alloc = doc.GetAllocator();
  doc.AddMember("path", rapidjson::Value(input.path.c_str(), alloc).Move(),
                alloc);
  doc.AddMember("mode", rapidjson::Value("line_range_edits", alloc).Move(),
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
      if (norm.description.find(edit.op) != std::string::npos) {
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
  return doc;
}

FileEditTargetInput parseFileEditTarget(const rapidjson::Value &json) {
  FileEditTargetInput input;

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
      if (value.HasMember("old_string") && value["old_string"].IsString()) {
        edit.has_old_string = true;
        edit.old_string = value["old_string"].GetString();
      }
      if (value.HasMember("new_string") && value["new_string"].IsString()) {
        edit.has_new_string = true;
        edit.new_string = value["new_string"].GetString();
      }
      if (value.HasMember("replace_all") && value["replace_all"].IsBool()) {
        edit.replace_all = value["replace_all"].GetBool();
      }
      if (value.HasMember("patch_line") && value["patch_line"].IsInt()) {
        edit.patch_line = value["patch_line"].GetInt();
      }
      edit.new_lines = readStringArray(value, "new_lines");
      if (hasMeaningfulEditInput(edit)) {
        input.edits.push_back(std::move(edit));
      }
    }
  }

  if (json.HasMember("patch") && json["patch"].IsString()) {
    input.has_patch = true;
    input.patch = json["patch"].GetString();
  }
  return input;
}

std::optional<std::string>
preflightValidateFileEditTarget(const FileEditTargetInput &input,
                                shared::ToolContext &ctx) {
  if (input.path.empty()) {
    return std::string("file_edit requires a path for each target file.");
  }

  const std::string absolutePath =
      ctx.agent.getEnvironment()->getWorkspace().resolvePath(input.path);
  const bool fileExists = ctx.host.exists(absolutePath);
  const StructuredEditMode structuredEditMode =
      classifyStructuredEditMode(input.edits);
  const bool hasAnchorEdits =
      structuredEditMode == StructuredEditMode::LineRange;
  const bool hasSearchReplaceEdits =
      structuredEditMode == StructuredEditMode::SearchReplace;
  const bool hasLegacyReplace = hasMeaningfulLegacyReplace(input);
  const bool hasPatch = hasMeaningfulPatch(input);
  const bool hasMeaningfulContentPayload =
      input.has_content && !input.content.empty();
  const bool hasContentEnvelope =
      hasContentOnlyEnvelope(input, hasAnchorEdits || hasSearchReplaceEdits,
                             hasLegacyReplace, hasPatch);
  const bool hasEmptyCreate =
      hasContentEnvelope && input.content.empty() && !fileExists;
  const int modeCount =
      static_cast<int>(hasAnchorEdits) +
      static_cast<int>(hasSearchReplaceEdits) +
      static_cast<int>(hasMeaningfulContentPayload) +
      static_cast<int>(hasLegacyReplace) + static_cast<int>(hasPatch) +
      static_cast<int>(hasEmptyCreate);

  if (structuredEditMode == StructuredEditMode::Mixed) {
    return std::string(
        "Do not mix search_replace edits with line-range edits in the "
        "same target file. Use separate file entries or separate file_edit "
        "calls.");
  }

  if (modeCount > 1) {
    return std::string(
        "file_edit accepts one editing mode per target file. Choose either "
        "line-range edits, search_replace edits, whole-file content, patch "
        "mode, or legacy old_string/new_string compatibility mode.");
  }

  if (modeCount == 0) {
    if (hasContentEnvelope && input.content.empty() && fileExists) {
      return std::string(
          "Missing edits, content, patch, or legacy replacement parameters. "
          "Ignored inert content:\"\" for the existing file. To clear a "
          "file, use an explicit patch or line-range edit after reading it.");
    }
    return std::string(
        "Missing edits, content, patch, or legacy replacement parameters.");
  }

  if (hasPatch) {
    try {
      (void)PatchParser::parse(input.patch);
    } catch (const std::exception &e) {
      return std::string(e.what());
    }
  }

  if ((hasAnchorEdits || hasPatch) && !fileExists) {
    return std::string(hasPatch ? "Patch mode requires an existing file."
                                : "Line-range edits require an existing file. "
                                  "Use content when creating a new file.");
  }

  if (hasSearchReplaceEdits && !fileExists) {
    return std::string(
        "search_replace edits require an existing file. Use content when "
        "creating a new file.");
  }

  if ((hasMeaningfulContentPayload || hasEmptyCreate) && fileExists &&
      !ctx.agent.getEnvironment()->getWorkspace().hasFullyReadFile(
          absolutePath)) {
    return std::string(
        "Refusing to overwrite an existing file that has not been fully "
        "read in this thread. Read the full file with 'file_read' before "
        "using content overwrite mode.");
  }

  return std::nullopt;
}

rapidjson::Document
executeSearchReplaceEditsDoc(const FileEditTargetInput &input,
                             const std::string &absolutePath,
                             shared::ToolContext &ctx,
                             std::unique_ptr<rapidjson::Document> *beforeLspOut =
                                 nullptr) {
  rapidjson::Document doc;
  doc.SetObject();
  auto &alloc = doc.GetAllocator();
  std::string currentContent;
  {
    auto data = ctx.host.readFile(absolutePath);
    currentContent = std::string(data.begin(), data.end());
  }

  std::string newContent = currentContent;
  int totalReplacements = 0;
  int addedLines = 0;
  int removedLines = 0;
  std::vector<NormalizedEdit> normalizedEdits;

  for (const auto &edit : input.edits) {
    if (edit.op == "search_replace" && edit.has_old_string &&
        edit.has_new_string) {
      const std::string beforeEdit = newContent;
      std::string searchStr = edit.old_string;
      std::string replaceStr = edit.new_string;
      size_t pos = 0;
      int replacements = 0;

      if (edit.replace_all) {
        while ((pos = newContent.find(searchStr, pos)) != std::string::npos) {
          newContent.replace(pos, searchStr.size(), replaceStr);
          pos += replaceStr.size();
          replacements++;
          totalReplacements++;
        }
      } else {
        pos = newContent.find(searchStr);
        if (pos != std::string::npos) {
          newContent.replace(pos, searchStr.size(), replaceStr);
          replacements++;
          totalReplacements++;
        }
      }

      if (replacements == 0 && !edit.replace_all) {
        doc.AddMember("error",
                      rapidjson::Value(
                          ("Could not find \"" + edit.old_string +
                           "\" in the file. Ensure the file content matches.")
                              .c_str(),
                          alloc),
                      alloc);
        return doc;
      }

      if (auto preview = buildSearchReplacePreview(beforeEdit, newContent, edit)) {
        addedLines += static_cast<int>(preview->newLines.size());
        removedLines += static_cast<int>(preview->oldLines.size());
        normalizedEdits.push_back(std::move(*preview));
      }
    }
  }

  if (beforeLspOut != nullptr) {
    *beforeLspOut = std::make_unique<rapidjson::Document>(
        collectFileLspDiagnostics(absolutePath, ctx));
  }
  ctx.host.writeFile(
      absolutePath, std::vector<uint8_t>(newContent.begin(), newContent.end()));
  ctx.agent.getEnvironment()->getWorkspace().recordFileEdit(absolutePath);

  doc.AddMember("path", rapidjson::Value(input.path.c_str(), alloc).Move(),
                alloc);
  doc.AddMember("mode", rapidjson::Value("search_replace_edits", alloc).Move(),
                alloc);
  doc.AddMember("replacements", totalReplacements, alloc);
  doc.AddMember("applied_edits",
                static_cast<uint32_t>(normalizedEdits.size()), alloc);
  doc.AddMember("added_lines", addedLines, alloc);
  doc.AddMember("removed_lines", removedLines, alloc);
  const std::string diffPreview = buildDiffPreview(normalizedEdits);
  doc.AddMember("diff_preview",
                rapidjson::Value(diffPreview.c_str(), alloc).Move(), alloc);
  rapidjson::Value operations(rapidjson::kArrayType);
  for (const auto &edit : normalizedEdits) {
    operations.PushBack(buildOperationResult(edit, alloc), alloc);
  }
  doc.AddMember("operations", operations, alloc);
  doc.AddMember("watch_state", rapidjson::Value("refreshed", alloc).Move(),
                alloc);

  return doc;
}

rapidjson::Document executeLegacyReplaceDoc(const FileEditTargetInput &input,
                                            const std::string &absolutePath,
                                            shared::ToolContext &ctx,
                                            std::unique_ptr<rapidjson::Document> *beforeLspOut =
                                                nullptr) {
  rapidjson::Document doc;
  doc.SetObject();
  auto &alloc = doc.GetAllocator();

  std::string currentContent;
  {
    auto data = ctx.host.readFile(absolutePath);
    currentContent = std::string(data.begin(), data.end());
  }

  std::string newContent = currentContent;
  size_t pos = newContent.find(input.old_string);

  if (pos == std::string::npos) {
    doc.AddMember("error",
                  rapidjson::Value(("Could not find \"" + input.old_string +
                                    "\" in the file.")
                                       .c_str(),
                                   alloc),
                  alloc);
    return doc;
  }

  newContent.replace(pos, input.old_string.size(), input.new_string);

  if (beforeLspOut != nullptr) {
    *beforeLspOut = std::make_unique<rapidjson::Document>(
        collectFileLspDiagnostics(absolutePath, ctx));
  }
  ctx.host.writeFile(
      absolutePath, std::vector<uint8_t>(newContent.begin(), newContent.end()));
  ctx.agent.getEnvironment()->getWorkspace().recordFileEdit(absolutePath);

  doc.AddMember("path", rapidjson::Value(input.path.c_str(), alloc).Move(),
                alloc);
  doc.AddMember("mode", rapidjson::Value("legacy_replace", alloc).Move(),
                alloc);
  doc.AddMember("watch_state", rapidjson::Value("refreshed", alloc).Move(),
                alloc);

  return doc;
}

FileEditExecutionResult executeSingleFileEdit(const FileEditTargetInput &input,
                                              shared::ToolContext &ctx) {
  FileEditExecutionResult result;
  result.doc.SetObject();

  if (input.path.empty()) {
    result.failureMessage = "file_edit requires a path for each target file.";
    return result;
  }

  std::string absolutePath =
      ctx.agent.getEnvironment()->getWorkspace().resolvePath(input.path);
  const bool fileExists = ctx.host.exists(absolutePath);
  const StructuredEditMode structuredEditMode =
      classifyStructuredEditMode(input.edits);
  const bool hasAnchorEdits =
      structuredEditMode == StructuredEditMode::LineRange;
  const bool hasSearchReplaceEdits =
      structuredEditMode == StructuredEditMode::SearchReplace;
  const bool hasLegacyReplace = hasMeaningfulLegacyReplace(input);
  const bool hasPatch = hasMeaningfulPatch(input);
  const bool hasMeaningfulContentPayload =
      input.has_content && !input.content.empty();
  const bool hasContentEnvelope =
      hasContentOnlyEnvelope(input, hasAnchorEdits || hasSearchReplaceEdits,
                             hasLegacyReplace, hasPatch);
  const bool hasOverwrite =
      hasMeaningfulOverwrite(input, hasAnchorEdits || hasSearchReplaceEdits,
                             hasLegacyReplace, hasPatch);
  const bool hasEmptyCreate =
      hasContentEnvelope && input.content.empty() && !fileExists;
  const int modeCount =
      static_cast<int>(hasAnchorEdits) +
      static_cast<int>(hasSearchReplaceEdits) +
      static_cast<int>(hasMeaningfulContentPayload) +
      static_cast<int>(hasLegacyReplace) + static_cast<int>(hasPatch) +
      static_cast<int>(hasEmptyCreate);

  if (structuredEditMode == StructuredEditMode::Mixed) {
    result.failureMessage =
        "Do not mix search_replace edits with line-range edits in the "
        "same target file. Use separate file entries or separate file_edit "
        "calls.";
    return result;
  }

  if (modeCount > 1) {
    result.failureMessage =
        "file_edit accepts one editing mode per target file. Choose either "
        "line-range edits, search_replace edits, whole-file content, patch "
        "mode, or legacy "
        "old_string/new_string compatibility mode.";
    return result;
  }

  if (modeCount == 0) {
    if (hasContentEnvelope && input.content.empty() && fileExists) {
      result.failureMessage =
          "Missing edits, content, patch, or legacy replacement parameters. "
          "Ignored inert content:\"\" for the existing file. To clear a file, "
          "use an explicit patch or line-range edit after reading it.";
    } else {
      result.failureMessage =
          "Missing edits, content, patch, or legacy replacement parameters.";
    }
    return result;
  }

  FileEditTargetInput effectiveInput = input;
  if (hasPatch) {
    try {
      effectiveInput.edits = PatchParser::parse(input.patch);
    } catch (const std::exception &e) {
      result.failureMessage = e.what();
      return result;
    }
  }

  try {
    ctx.agent.getPermissions()->validatePathAccess(absolutePath,
                                                   AccessMode::WRITE);
    auto collectBeforeLsp = [&]() -> std::unique_ptr<rapidjson::Document> {
      if (!fileExists) {
        return nullptr;
      }
      return std::make_unique<rapidjson::Document>(
          collectFileLspDiagnostics(absolutePath, ctx));
    };

    if (hasAnchorEdits || hasPatch) {
      if (!fileExists) {
        result.failureMessage =
            (hasPatch ? "Patch mode requires an existing file."
                      : "Line-range edits require an existing file. Use "
                        "content when "
                        "creating a new file.");
        return result;
      }

      auto data = ctx.host.readFile(absolutePath);
      std::string content(data.begin(), data.end());
      FileBuffer buffer = splitFileContent(content);

      utils::LineRangeTrimmer::SanitationResult sanitation;
      NormalizationResult normResult =
          normalizeEdits(effectiveInput.edits, buffer.lines, &sanitation);
      if (!normResult.errors.empty()) {
        result.doc =
            buildLineRangeFailureDoc(effectiveInput, normResult, sanitation);
        if (hasPatch) {
          auto &alloc = result.doc.GetAllocator();
          if (result.doc.HasMember("batch_errors") &&
              result.doc["batch_errors"].IsArray()) {
            for (auto &err : result.doc["batch_errors"].GetArray()) {
              if (err.HasMember("edit_index") && err["edit_index"].IsUint()) {
                uint32_t idx = err["edit_index"].GetUint();
                if (idx < effectiveInput.edits.size()) {
                  err.AddMember("patch_line",
                                effectiveInput.edits[idx].patch_line, alloc);
                }
              }
            }
          }
        }
        result.failureMessage = result.doc["error"].GetString();
        return result;
      }

      const auto &normalized = normResult.normalized;
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

      auto beforeLsp = collectBeforeLsp();
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

      auto &alloc = result.doc.GetAllocator();
      result.doc.AddMember(
          "path", rapidjson::Value(input.path.c_str(), alloc).Move(), alloc);
      result.doc.AddMember(
          "mode",
          rapidjson::Value(hasPatch ? "patch" : "line_range_edits", alloc)
              .Move(),
          alloc);
      result.doc.AddMember("applied_edits",
                           static_cast<uint32_t>(normalized.size()), alloc);
      result.doc.AddMember("removed_lines", removedLines, alloc);
      result.doc.AddMember("added_lines", addedLines, alloc);
      result.doc.AddMember("relocated_anchors", relocatedAnchors, alloc);
      const std::string diffPreview = buildDiffPreview(normalized);
      result.doc.AddMember("diff_preview",
                           rapidjson::Value(diffPreview.c_str(), alloc).Move(),
                           alloc);
      result.doc.AddMember("watch_state",
                           rapidjson::Value("refreshed", alloc).Move(), alloc);

      rapidjson::Value operations(rapidjson::kArrayType);
      for (const auto &edit : normalized) {
        operations.PushBack(buildOperationResult(edit, alloc), alloc);
      }
      result.doc.AddMember("operations", operations, alloc);
      addSanitationMember(result.doc, sanitation, alloc);
      auto afterLsp = collectFileLspDiagnostics(absolutePath, ctx);
      attachFileEditLspSummary(result.doc, absolutePath, beforeLsp.get(),
                               &afterLsp);
      result.success = true;
      return result;
    }

    if (hasSearchReplaceEdits) {
      if (!fileExists) {
        result.failureMessage =
            "search_replace edits require an existing file. Use content when "
            "creating a new file.";
        return result;
      }

      std::unique_ptr<rapidjson::Document> beforeLsp;
      result.doc = executeSearchReplaceEditsDoc(effectiveInput, absolutePath,
                                                ctx, &beforeLsp);
      if (result.doc.HasMember("error") && result.doc["error"].IsString()) {
        result.failureMessage = result.doc["error"].GetString();
        return result;
      }
      auto afterLsp = collectFileLspDiagnostics(absolutePath, ctx);
      attachFileEditLspSummary(result.doc, absolutePath, beforeLsp.get(),
                               &afterLsp);
      result.success = true;
      return result;
    }

    if (hasOverwrite || hasEmptyCreate) {
      if (fileExists &&
          !ctx.agent.getEnvironment()->getWorkspace().hasFullyReadFile(
              absolutePath)) {
        result.failureMessage =
            "Refusing to overwrite an existing file that has not been fully "
            "read in this thread. Read the full file with 'file_read' before "
            "using content overwrite mode.";
        return result;
      }

      auto beforeLsp = collectBeforeLsp();
      ctx.host.writeFile(
          absolutePath,
          std::vector<uint8_t>(input.content.begin(), input.content.end()));
      ctx.agent.getEnvironment()->getWorkspace().recordFileEdit(absolutePath);

      auto &alloc = result.doc.GetAllocator();
      result.doc.AddMember(
          "path", rapidjson::Value(input.path.c_str(), alloc).Move(), alloc);
      result.doc.AddMember("mode", rapidjson::Value("overwrite", alloc).Move(),
                           alloc);
      result.doc.AddMember("bytes_written",
                           static_cast<uint32_t>(input.content.size()), alloc);
      result.doc.AddMember("watch_state",
                           rapidjson::Value("refreshed", alloc).Move(), alloc);
      auto afterLsp = collectFileLspDiagnostics(absolutePath, ctx);
      attachFileEditLspSummary(result.doc, absolutePath, beforeLsp.get(),
                               &afterLsp);
      result.success = true;
      return result;
    }

    std::unique_ptr<rapidjson::Document> beforeLsp;
    result.doc =
        executeLegacyReplaceDoc(effectiveInput, absolutePath, ctx, &beforeLsp);
    if (result.doc.HasMember("error") && result.doc["error"].IsString()) {
      result.failureMessage = result.doc["error"].GetString();
      return result;
    }
    auto afterLsp = collectFileLspDiagnostics(absolutePath, ctx);
    attachFileEditLspSummary(result.doc, absolutePath, beforeLsp.get(),
                             &afterLsp);
    result.success = true;
    return result;
  } catch (const std::exception &e) {
    result.failureMessage = e.what();
    return result;
  }
}

} // namespace

shared::ToolMetadata FileEditTool::getMetadata() const {
  return {"file_edit",
          "Edit one or more files with line-number anchors: read first, use "
          "exact line numbers, use small ops, and reread before the "
          "next edit call",
          ToolScope::FilesystemWrite};
}

std::shared_ptr<shared::JSONSchema> FileEditTool::getSchema() const {
  auto editSchema = zObject(
      {{"op", zEnum({"replace_range", "insert_after", "insert_before",
                     "delete_range", "search_replace"})
                  ->describe(
                      "Edit operation type. Line-range ops use exact "
                      "line-number anchors from file_read. search_replace uses "
                      "old_string/new_string and can set replace_all.")},
       {"start_anchor",
        zString()
            ->describe("Start anchor for range edits. Use the exact "
                       "line number returned by file_read. Do not "
                       "include trailing |content, and do not rewrite anchors "
                       "within the same call. Used only by range edits.")
            ->setOptional()},
       {"end_anchor",
        zString()
            ->describe("End anchor for range edits. Use the exact "
                       "line number returned by file_read. Do not "
                       "include trailing |content, and do not rewrite anchors "
                       "within the same call. Used only by range edits.")
            ->setOptional()},
       {"anchor",
        zString()
            ->describe("Single anchor for insert edits. Use the exact "
                       "line number returned by file_read. Do not "
                       "include trailing |content. Prefer structural lines "
                       "over blank lines when choosing anchors. Used only by "
                       "insert edits.")
            ->setOptional()},
       {"new_lines",
        zArray(zString())
            ->describe("Plain replacement or inserted source lines only, "
                       "without trailing newline characters. Do not include "
                       "line prefixes, trailing |content, diff markers, "
                       "or unchanged boundary lines from outside the edited "
                       "range. Used by line-range edit ops. Empty or missing "
                       "new_lines is fine for delete_range and inert empty "
                       "transport placeholders are ignored.")
            ->setOptional()},
       {"old_string",
        zString()
            ->describe("Literal text to replace when op is search_replace. "
                       "Read the full file first and make this specific "
                       "enough to avoid accidental matches.")
            ->setOptional()},
       {"new_string",
        zString()
            ->describe("Replacement text for search_replace. May be an empty "
                       "string if you intend to delete the matched text.")
            ->setOptional()},
       {"replace_all",
        zBoolean()
            ->describe("Optional for search_replace. When true, replace every "
                       "match in the file for that operation.")
            ->setOptional()}});

  auto fileSchema =
      zObject(
          {{"path",
            zString()->describe("Absolute or relative path to the file")},
           {"edits",
            zArray(editSchema)
                ->describe("Structured edits for one existing file. Use either "
                           "line-range ops or search_replace ops within a "
                           "single target file, not both. Empty edit arrays "
                           "or empty placeholder edit objects are ignored.")
                ->setOptional()},
           {"content",
            zString()
                ->describe(
                    "Whole-file content. Best for new-file creation, and "
                    "also allowed for full overwrites after the file has "
                    "been fully read. content:\"\" is meaningful for creating "
                    "a new empty file; for an existing file it is treated as "
                    "inert unless that is the only explicit intent.")
                ->setOptional()},
           {"patch",
            zString()
                ->describe(
                    "Patch content for patch mode. Use '@@ anchor @@' or "
                    "'@@ start...end @@' followed by '-' and '+' lines.")
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

  return zObject(
             {{"path",
               zString()->describe("Absolute or relative path to the file")},
              {"edits",
               zArray(editSchema)
                   ->describe("Structured edits for a single file. Use "
                              "line-range ops after file_read, or use "
                              "search_replace after reading the full file. Do "
                              "not mix the two styles within one target. "
                              "Empty edit arrays or empty placeholder edit "
                              "objects are ignored.")
                   ->setOptional()},
              {"content", zString()
                              ->describe("Whole-file content. Best for "
                                         "explicit new-file creation, and "
                                         "also valid for full overwrites after "
                                         "the file has been fully read. "
                                         "content:\"\" creates a new empty "
                                         "file but is otherwise inert noise "
                                         "unless it is the only explicit "
                                         "intent.")
                              ->setOptional()},
              {"patch",
               zString()
                   ->describe(
                       "Patch content for patch mode. Use '@@ anchor @@' or "
                       "'@@ start...end @@' followed by '-' and '+' lines.")
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
                   ->setOptional()},
              {"files",
               zArray(fileSchema)
                   ->describe("Optional multi-file request. Each entry uses "
                              "the same shape and rules as the single-file "
                              "form. Use either top-level path/... fields or "
                              "files[], not both. Semantically inert wrapper "
                              "defaults like empty edits, empty patch, and "
                              "empty legacy strings are ignored.")
                   ->setOptional()}})
      ->required({});
}

FileEditInput FileEditTool::transform(const rapidjson::Value &json) {
  FileEditInput input;
  static_cast<FileEditTargetInput &>(input) = parseFileEditTarget(json);
  if (json.HasMember("files") && json["files"].IsArray()) {
    for (const auto &value : json["files"].GetArray()) {
      if (!value.IsObject()) {
        continue;
      }
      input.files.push_back(parseFileEditTarget(value));
    }
  }
  return input;
}

shared::ToolResult FileEditTool::execute(const FileEditInput &input,
                                         shared::ToolContext &ctx) {
  FileEditInput normalized = input;
  sanitizeTargetInput(normalized);
  for (auto &target : normalized.files) {
    sanitizeTargetInput(target);
  }
  normalized.files.erase(
      std::remove_if(normalized.files.begin(), normalized.files.end(),
                     [](const FileEditTargetInput &target) {
                       return !hasMeaningfulTargetEnvelope(target);
                     }),
      normalized.files.end());

  const bool hasMultiFile = !normalized.files.empty();
  const bool hasTopLevelPayload =
      hasMeaningfulTopLevelPayloadForMultiFile(normalized);

  if (hasMultiFile && hasTopLevelPayload) {
    return shared::ToolResult::fail(buildMultiFileMixedModeError(normalized));
  }

  if (!hasMultiFile) {
    const auto single = executeSingleFileEdit(normalized, ctx);
    if (!single.success) {
      if (!single.doc.ObjectEmpty()) {
        rapidjson::StringBuffer sb;
        rapidjson::Writer<rapidjson::StringBuffer> writer(sb);
        single.doc.Accept(writer);
        return shared::ToolResult::fail(sb.GetString());
      }
      return shared::ToolResult::fail(single.failureMessage);
    }
    return shared::ToolResult::ok(single.doc);
  }

  rapidjson::Document aggregate;
  aggregate.SetObject();
  auto &alloc = aggregate.GetAllocator();
  aggregate.AddMember("mode", rapidjson::Value("multi_file", alloc).Move(),
                      alloc);

  rapidjson::Value files(rapidjson::kArrayType);
  rapidjson::Value editedPaths(rapidjson::kArrayType);
  uint32_t successCount = 0;
  uint32_t appliedEdits = 0;
  int addedLines = 0;
  int removedLines = 0;

  for (const auto &target : normalized.files) {
    if (auto validationError = preflightValidateFileEditTarget(target, ctx)) {
      return shared::ToolResult::fail(*validationError);
    }
  }

  for (const auto &target : normalized.files) {
    auto fileResult = executeSingleFileEdit(target, ctx);
    if (!fileResult.success) {
      if (!fileResult.doc.ObjectEmpty()) {
        rapidjson::StringBuffer sb;
        rapidjson::Writer<rapidjson::StringBuffer> writer(sb);
        fileResult.doc.Accept(writer);
        return shared::ToolResult::fail(sb.GetString());
      }
      return shared::ToolResult::fail(fileResult.failureMessage);
    }

    if (fileResult.doc.HasMember("path") && fileResult.doc["path"].IsString()) {
      editedPaths.PushBack(
          rapidjson::Value(fileResult.doc["path"].GetString(), alloc).Move(),
          alloc);
    }
    if (fileResult.doc.HasMember("applied_edits") &&
        fileResult.doc["applied_edits"].IsUint()) {
      appliedEdits += fileResult.doc["applied_edits"].GetUint();
    }
    if (fileResult.doc.HasMember("added_lines") &&
        fileResult.doc["added_lines"].IsInt()) {
      addedLines += fileResult.doc["added_lines"].GetInt();
    }
    if (fileResult.doc.HasMember("removed_lines") &&
        fileResult.doc["removed_lines"].IsInt()) {
      removedLines += fileResult.doc["removed_lines"].GetInt();
    }

    files.PushBack(cloneJsonValue(fileResult.doc, alloc), alloc);
    ++successCount;
  }

  aggregate.AddMember("files", files, alloc);
  aggregate.AddMember("edited_files", editedPaths, alloc);
  aggregate.AddMember("file_count", successCount, alloc);
  aggregate.AddMember("applied_edits", appliedEdits, alloc);
  aggregate.AddMember("added_lines", addedLines, alloc);
  aggregate.AddMember("removed_lines", removedLines, alloc);
  aggregate.AddMember("watch_state",
                      rapidjson::Value("refreshed", alloc).Move(), alloc);
  return shared::ToolResult::ok(aggregate);
}

} // namespace firmius::core

#include "tools/FileEditTool.hpp"

#include "agents/Agent.hpp"
#include "lsp/LspService.hpp"
#include "utils/LineRange.hpp"
#include "utils/StringUtil.hpp"

#include <algorithm>
#include <filesystem>
#include <optional>
#include <rapidjson/document.h>
#include <regex>
#include <sstream>
#include <stdexcept>

namespace firmius::core {
using namespace firmius::shared;

namespace {

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

struct PreparedFileMutation {
  std::string requestPath;
  std::string absolutePath;
  std::string mode;
  std::string updatedContent;
  std::vector<NormalizedEdit> normalizedEdits;
  utils::LineRangeTrimmer::SanitationResult sanitation;
  std::unique_ptr<rapidjson::Document> beforeLsp;
  int addedLines = 0;
  int removedLines = 0;
  int relocatedAnchors = 0;
  int replacements = 0;
};

rapidjson::Value cloneJsonValue(const rapidjson::Value &value,
                                rapidjson::Document::AllocatorType &alloc) {
  rapidjson::Value clone;
  clone.CopyFrom(value, alloc);
  return clone;
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

std::vector<std::string> splitLinesSimple(const std::string &content) {
  std::vector<std::string> lines;
  std::stringstream ss(content);
  std::string line;
  while (std::getline(ss, line)) {
    lines.push_back(line);
  }
  return lines;
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
  if (!edit.anchor.empty()) {
    throw std::runtime_error(edit.op +
                             " requires both start_anchor and end_anchor; got anchor only");
  }
  if (missingFields.size() == 1) {
    throw std::runtime_error(edit.op + " is missing " + missingFields.front());
  }
  throw std::runtime_error(edit.op + " requires both start_anchor and end_anchor");
}

void requireSingleAnchor(const FileEditOperationInput &edit) {
  if (edit.anchor.empty()) {
    throw std::runtime_error(edit.op + " requires anchor");
  }
}

void validateRangeEditOperation(const FileEditOperationInput &edit) {
  if (edit.op == "replace_range" || edit.op == "delete_range") {
    requireRangeAnchors(edit);
    return;
  }
  if (edit.op == "insert_after" || edit.op == "insert_before") {
    requireSingleAnchor(edit);
    return;
  }
  throw std::runtime_error("Malformed range op '" + edit.op +
                           "'. Expected replace_range, insert_after, "
                           "insert_before, or delete_range.");
}

void validateReplacementOperation(const FileEditOperationInput &edit) {
  if (edit.op != "search_replace") {
    throw std::runtime_error("Malformed replacement op '" + edit.op +
                             "'. Expected search_replace.");
  }
  if (!edit.has_old_string) {
    throw std::runtime_error("search_replace requires old_string");
  }
  if (!edit.has_new_string) {
    throw std::runtime_error("search_replace requires new_string");
  }
  if (edit.old_string.empty()) {
    throw std::runtime_error("search_replace old_string cannot be empty");
  }
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
                             beforeLines.begin() + static_cast<long>(beforeSuffix));
  normalized.newLines.assign(afterLines.begin() + static_cast<long>(prefix),
                             afterLines.begin() + static_cast<long>(afterSuffix));
  return normalized;
}

bool editsConflict(const NormalizedEdit &left, const NormalizedEdit &right) {
  const bool leftInsertion = left.startIndex == left.endIndex;
  const bool rightInsertion = right.startIndex == right.endIndex;

  if (leftInsertion && rightInsertion) {
    return left.startIndex == right.startIndex;
  }
  if (leftInsertion) {
    return left.startIndex >= right.startIndex && left.startIndex <= right.endIndex;
  }
  if (rightInsertion) {
    return right.startIndex >= left.startIndex && right.startIndex <= left.endIndex;
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
  totals->malformedHashFragmentsStripped += delta.malformedHashFragmentsStripped;
  totals->diffMarkersStripped += delta.diffMarkersStripped;
  totals->boundaryEchoesRemoved += delta.boundaryEchoesRemoved;
  totals->boundaryEchoRemoved = totals->boundaryEchoRemoved || delta.boundaryEchoRemoved;
  totals->suspiciousContentFound = totals->suspiciousContentFound || delta.suspiciousContentFound;
  totals->suspiciousContentRejected = totals->suspiciousContentRejected || delta.suspiciousContentRejected;
}

std::vector<std::string>
sanitizeReplacementLines(const FileEditOperationInput &edit,
                         utils::LineRangeTrimmer::SanitationResult *sanitation) {
  std::vector<std::string> sanitized;
  sanitized.reserve(edit.new_lines.size());

  for (const auto &rawLine : edit.new_lines) {
    utils::LineRangeTrimmer::SanitationResult lineSanitation;
    std::string line = utils::LineRangeTrimmer::sanitizeContent(rawLine, &lineSanitation);
    if (utils::LineRangeTrimmer::startsWithSuspiciousMetadata(line)) {
      lineSanitation.suspiciousContentFound = true;
      lineSanitation.suspiciousContentRejected = true;
      mergeSanitation(sanitation, lineSanitation);
      throw std::runtime_error(
          "Replacement text still appears to contain Hashline metadata. "
          "Remove lineNumber#hash| prefixes or trailing hash fragments from new_lines.");
    }
    if (utils::LineRangeTrimmer::startsWithSuspiciousDiffJunk(line)) {
      lineSanitation.suspiciousContentFound = true;
      lineSanitation.suspiciousContentRejected = true;
      mergeSanitation(sanitation, lineSanitation);
      throw std::runtime_error(
          "Replacement text still appears to contain diff markers. Remove leading + / - patch markers from new_lines.");
    }
    mergeSanitation(sanitation, lineSanitation);
    sanitized.push_back(std::move(line));
  }

  return sanitized;
}

void stripBoundaryEchoes(std::vector<std::string> &newLines,
                         const std::vector<std::string> &lines, int startIndex,
                         int endIndexExclusive,
                         utils::LineRangeTrimmer::SanitationResult *sanitation) {
  if (!newLines.empty() && startIndex > 0 && newLines.front() == lines[startIndex - 1]) {
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

void addSanitationMember(rapidjson::Document &doc,
                         const utils::LineRangeTrimmer::SanitationResult &sanitation,
                         rapidjson::Document::AllocatorType &alloc) {
  rapidjson::Value value(rapidjson::kObjectType);
  value.AddMember("line_range_prefixes_stripped", sanitation.lineRangePrefixesStripped, alloc);
  value.AddMember("hashline_prefixes_stripped", sanitation.hashlinePrefixesStripped, alloc);
  value.AddMember("malformed_hash_fragments_stripped", sanitation.malformedHashFragmentsStripped, alloc);
  value.AddMember("diff_markers_stripped", sanitation.diffMarkersStripped, alloc);
  value.AddMember("boundary_echoes_removed", sanitation.boundaryEchoesRemoved, alloc);
  value.AddMember("boundary_echo_removed", sanitation.boundaryEchoRemoved, alloc);
  value.AddMember("suspicious_content_found", sanitation.suspiciousContentFound, alloc);
  value.AddMember("suspicious_content_rejected", sanitation.suspiciousContentRejected, alloc);
  doc.AddMember("sanitation", value, alloc);
}

rapidjson::Value buildOperationResult(const NormalizedEdit &edit,
                                      rapidjson::Document::AllocatorType &alloc) {
  rapidjson::Value value(rapidjson::kObjectType);
  value.AddMember("op", rapidjson::Value(edit.op.c_str(), alloc).Move(), alloc);
  value.AddMember("description", rapidjson::Value(edit.description.c_str(), alloc).Move(), alloc);
  value.AddMember("start_line", edit.startIndex + 1, alloc);
  value.AddMember("end_line", edit.endIndex, alloc);
  value.AddMember("new_line_count", static_cast<uint32_t>(edit.newLines.size()), alloc);
  value.AddMember("old_line_count", static_cast<uint32_t>(edit.oldLines.size()), alloc);
  value.AddMember("relocated", edit.relocated, alloc);
  rapidjson::Value oldLines(rapidjson::kArrayType);
  for (const auto &line : edit.oldLines) {
    oldLines.PushBack(rapidjson::Value(line.c_str(), alloc).Move(), alloc);
  }
  rapidjson::Value newLines(rapidjson::kArrayType);
  for (const auto &line : edit.newLines) {
    newLines.PushBack(rapidjson::Value(line.c_str(), alloc).Move(), alloc);
  }
  value.AddMember("old_lines", oldLines, alloc);
  value.AddMember("new_lines", newLines, alloc);
  return value;
}

std::string buildDiffPreview(const std::vector<NormalizedEdit> &normalized) {
  std::ostringstream out;
  for (const auto &edit : normalized) {
    out << "@@ " << edit.description << " @@\n";
    for (const auto &line : edit.oldLines) {
      out << '-' << line << '\n';
    }
    for (const auto &line : edit.newLines) {
      out << '+' << line << '\n';
    }
  }
  return out.str();
}

NormalizationResult normalizeRangeEdits(
    const std::vector<FileEditOperationInput> &edits,
    const std::vector<std::string> &lines,
    utils::LineRangeTrimmer::SanitationResult *sanitation = nullptr) {
  NormalizationResult result;

  for (size_t i = 0; i < edits.size(); ++i) {
    const auto &edit = edits[i];
    try {
      validateRangeEditOperation(edit);
      FileEditOperationInput cleanEdit = edit;
      cleanEdit.new_lines = sanitizeReplacementLines(edit, sanitation);

      if (edit.op == "replace_range") {
        auto start = utils::LineRange::resolveAnchor(lines, edit.start_anchor);
        auto end = utils::LineRange::resolveAnchor(lines, edit.end_anchor);
        if (start.status != utils::AnchorResult::Status::SUCCESS ||
            end.status != utils::AnchorResult::Status::SUCCESS) {
          result.errors.push_back(NormalizationError{
              i,
              start.status != utils::AnchorResult::Status::SUCCESS ? start : end,
              start.status != utils::AnchorResult::Status::SUCCESS ? start.errorMessage
                                                                   : end.errorMessage});
          continue;
        }
        stripBoundaryEchoes(cleanEdit.new_lines, lines, start.lineIndex, end.lineIndex + 1,
                            sanitation);
        result.normalized.push_back({edit.op,
                                     start.lineIndex,
                                     end.lineIndex + 1,
                                     cleanEdit.new_lines,
                                     describeEdit(edit),
                                     start.relocated || end.relocated,
                                     std::vector<std::string>(lines.begin() + start.lineIndex,
                                                              lines.begin() + end.lineIndex + 1)});
      } else if (edit.op == "delete_range") {
        auto start = utils::LineRange::resolveAnchor(lines, edit.start_anchor);
        auto end = utils::LineRange::resolveAnchor(lines, edit.end_anchor);
        if (start.status != utils::AnchorResult::Status::SUCCESS ||
            end.status != utils::AnchorResult::Status::SUCCESS) {
          result.errors.push_back(NormalizationError{
              i,
              start.status != utils::AnchorResult::Status::SUCCESS ? start : end,
              start.status != utils::AnchorResult::Status::SUCCESS ? start.errorMessage
                                                                   : end.errorMessage});
          continue;
        }
        result.normalized.push_back({edit.op,
                                     start.lineIndex,
                                     end.lineIndex + 1,
                                     {},
                                     describeEdit(edit),
                                     start.relocated || end.relocated,
                                     std::vector<std::string>(lines.begin() + start.lineIndex,
                                                              lines.begin() + end.lineIndex + 1)});
      } else if (edit.op == "insert_after") {
        auto anchor = utils::LineRange::resolveAnchor(lines, edit.anchor);
        if (anchor.status != utils::AnchorResult::Status::SUCCESS) {
          result.errors.push_back(
              NormalizationError{i, anchor, anchor.errorMessage});
          continue;
        }
        result.normalized.push_back({edit.op,
                                     anchor.lineIndex + 1,
                                     anchor.lineIndex + 1,
                                     cleanEdit.new_lines,
                                     describeEdit(edit),
                                     anchor.relocated,
                                     {}});
      } else if (edit.op == "insert_before") {
        auto anchor = utils::LineRange::resolveAnchor(lines, edit.anchor);
        if (anchor.status != utils::AnchorResult::Status::SUCCESS) {
          result.errors.push_back(
              NormalizationError{i, anchor, anchor.errorMessage});
          continue;
        }
        result.normalized.push_back({edit.op,
                                     anchor.lineIndex,
                                     anchor.lineIndex,
                                     cleanEdit.new_lines,
                                     describeEdit(edit),
                                     anchor.relocated,
                                     {}});
      }
    } catch (const std::exception &e) {
      utils::AnchorResult invalid;
      invalid.status = utils::AnchorResult::Status::MALFORMED;
      invalid.errorMessage = e.what();
      result.errors.push_back(NormalizationError{i, invalid, e.what()});
    }
  }

  std::sort(result.normalized.begin(), result.normalized.end(),
            [](const NormalizedEdit &left, const NormalizedEdit &right) {
              if (left.startIndex != right.startIndex) {
                return left.startIndex < right.startIndex;
              }
              return left.endIndex < right.endIndex;
            });
  for (size_t i = 1; i < result.normalized.size(); ++i) {
    if (editsConflict(result.normalized[i - 1], result.normalized[i])) {
      utils::AnchorResult invalid;
      invalid.status = utils::AnchorResult::Status::AMBIGUOUS;
      invalid.errorMessage = "Range edits overlap or target the same insertion point";
      result.errors.push_back(
          NormalizationError{i, invalid, invalid.errorMessage});
      break;
    }
  }
  return result;
}

rapidjson::Document buildRangeFailureDoc(
    const std::string &path, const std::vector<FileEditOperationInput> &edits,
    const NormalizationResult &normResult,
    const utils::LineRangeTrimmer::SanitationResult &sanitation,
    std::string_view resolved_mode = "range",
    std::string_view error_message =
        "One or more range operations failed validation.",
    std::string_view suggestion = "") {
  rapidjson::Document doc;
  doc.SetObject();
  auto &alloc = doc.GetAllocator();
  doc.AddMember("path", rapidjson::Value(path.c_str(), alloc).Move(), alloc);
  doc.AddMember("resolved_mode",
                rapidjson::Value(std::string(resolved_mode).c_str(), alloc).Move(),
                alloc);
  doc.AddMember("error",
                rapidjson::Value(std::string(error_message).c_str(), alloc).Move(),
                alloc);
  if (!suggestion.empty()) {
    doc.AddMember("suggestion",
                  rapidjson::Value(std::string(suggestion).c_str(), alloc).Move(),
                  alloc);
  }
  rapidjson::Value batchErrors(rapidjson::kArrayType);
  for (const auto &error : normResult.errors) {
    rapidjson::Value item(rapidjson::kObjectType);
    item.AddMember("edit_index", static_cast<uint32_t>(error.editIndex), alloc);
    item.AddMember("message", rapidjson::Value(error.errorMessage.c_str(), alloc).Move(), alloc);
    if (error.editIndex < edits.size()) {
      item.AddMember("description",
                     rapidjson::Value(describeEdit(edits[error.editIndex]).c_str(), alloc).Move(),
                     alloc);
      if (edits[error.editIndex].patch_line > 0) {
        item.AddMember("patch_line", edits[error.editIndex].patch_line, alloc);
      }
    }
    batchErrors.PushBack(item, alloc);
  }
  doc.AddMember("batch_errors", batchErrors, alloc);
  addSanitationMember(doc, sanitation, alloc);
  return doc;
}

std::string trimDiffPathPrefix(std::string path) {
  if (path == "/dev/null") {
    return path;
  }
  if (path.rfind("a/", 0) == 0 || path.rfind("b/", 0) == 0) {
    return path.substr(2);
  }
  return path;
}

class PatchParser {
public:
  struct Hunk {
    int startPatchLine = 0;
    int oldStartLine = 0;
    int oldCount = 0;
    int newStartLine = 0;
    int newCount = 0;
    std::string startAnchor;
    std::string endAnchor;
    std::vector<std::string> oldLines;
    std::vector<std::string> newLines;
    std::vector<std::string> rawLines;
  };

  static std::vector<FileEditOperationInput> parse(const std::string &patchText) {
    std::vector<FileEditOperationInput> edits;
    std::istringstream stream(patchText);
    std::string line;
    int lineNo = 0;
    std::vector<Hunk> hunks;
    static const std::regex githubPattern(R"(^@@\s+-(\d+)(?:,(\d+))?\s+\+(\d+)(?:,(\d+))?\s+@@.*)");
    static const std::regex anchorPattern(R"(^@@\s+([^\s]+?)(?:\.\.\.([^\s]+?))?\s+@@$)");
    Hunk *currentHunk = nullptr;
    bool inHunk = false;

    while (std::getline(stream, line)) {
      lineNo++;
      if (line.rfind("diff --git ", 0) == 0) continue;
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
        continue;
      }
      if (std::regex_match(line, match, anchorPattern)) {
        hunks.push_back({});
        currentHunk = &hunks.back();
        currentHunk->startPatchLine = lineNo;
        currentHunk->startAnchor = match[1].str();
        currentHunk->endAnchor = match[2].matched ? match[2].str() : "";
        inHunk = true;
        continue;
      }
      if (!inHunk) {
        throw std::runtime_error("Malformed patch at line " + std::to_string(lineNo) +
                                 ": expected unified hunk header");
      }
      if (line.empty()) {
        currentHunk->rawLines.push_back("+");
        currentHunk->newLines.push_back("");
        continue;
      }
      currentHunk->rawLines.push_back(line);
      char prefix = line[0];
      std::string content = line.substr(1);
      if (prefix == '-') {
        currentHunk->oldLines.push_back(content);
      } else if (prefix == '+') {
        currentHunk->newLines.push_back(content);
      } else if (prefix == ' ' || prefix == '\\') {
        continue;
      } else {
        throw std::runtime_error("Malformed patch at line " + std::to_string(lineNo) +
                                 ": unexpected patch line prefix");
      }
    }

    for (const auto &hunk : hunks) {
      if (!hunk.startAnchor.empty()) {
        FileEditOperationInput edit;
        edit.patch_line = hunk.startPatchLine;
        if (hunk.oldLines.empty() && hunk.newLines.empty()) {
          throw std::runtime_error("Malformed patch: hunk at line " +
                                   std::to_string(hunk.startPatchLine) + " has no changes");
        }
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
        edits.push_back(std::move(edit));
        continue;
      }

      int oldLineCursor = hunk.oldStartLine;
      bool inChange = false;
      int changeOldStart = 0;
      std::vector<std::string> changeOldLines;
      std::vector<std::string> changeNewLines;
      auto flush = [&]() {
        if (!inChange) return;
        FileEditOperationInput edit;
        edit.patch_line = hunk.startPatchLine;
        if (changeOldLines.empty()) {
          if (changeOldStart <= 1) {
            edit.op = "insert_before";
            edit.anchor = "1";
          } else {
            edit.op = "insert_after";
            edit.anchor = std::to_string(changeOldStart - 1);
          }
        } else if (changeNewLines.empty()) {
          edit.op = "delete_range";
          edit.start_anchor = std::to_string(changeOldStart);
          edit.end_anchor = std::to_string(changeOldStart + static_cast<int>(changeOldLines.size()) - 1);
        } else {
          edit.op = "replace_range";
          edit.start_anchor = std::to_string(changeOldStart);
          edit.end_anchor = std::to_string(changeOldStart + static_cast<int>(changeOldLines.size()) - 1);
        }
        edit.new_lines = changeNewLines;
        edits.push_back(std::move(edit));
        inChange = false;
        changeOldLines.clear();
        changeNewLines.clear();
      };

      for (const auto &rawLine : hunk.rawLines) {
        if (rawLine.empty()) continue;
        const char prefix = rawLine[0];
        const std::string content = rawLine.size() > 1 ? rawLine.substr(1) : "";
        if (prefix == ' ') {
          flush();
          oldLineCursor++;
        } else if (prefix == '-') {
          if (!inChange) {
            inChange = true;
            changeOldStart = oldLineCursor;
          }
          changeOldLines.push_back(content);
          oldLineCursor++;
        } else if (prefix == '+') {
          if (!inChange) {
            inChange = true;
            changeOldStart = oldLineCursor;
          }
          changeNewLines.push_back(content);
        }
      }
      flush();
    }
    return edits;
  }
};

struct PatchTarget {
  std::string path;
  std::string patch;
  std::vector<FileEditOperationInput> edits;
};

std::vector<PatchTarget> splitUnifiedPatchTargets(const std::string &patchText) {
  std::vector<PatchTarget> targets;
  std::istringstream stream(patchText);
  std::string line;
  std::vector<std::string> currentLines;
  std::string currentOldPath;
  std::string currentNewPath;
  bool hasCurrentFile = false;

  auto flush = [&]() {
    if (!hasCurrentFile) return;
    std::ostringstream joined;
    for (size_t i = 0; i < currentLines.size(); ++i) {
      if (i > 0) joined << '\n';
      joined << currentLines[i];
    }
    const std::string newPath = trimDiffPathPrefix(currentNewPath);
    const std::string oldPath = trimDiffPathPrefix(currentOldPath);
    PatchTarget target;
    target.path = !newPath.empty() && newPath != "/dev/null" ? newPath : oldPath;
    target.patch = joined.str();
    target.edits = PatchParser::parse(target.patch);
    targets.push_back(std::move(target));
    currentLines.clear();
    currentOldPath.clear();
    currentNewPath.clear();
    hasCurrentFile = false;
  };

  while (std::getline(stream, line)) {
    if (line.rfind("diff --git ", 0) == 0) {
      if (hasCurrentFile) flush();
      currentLines.push_back(line);
      continue;
    }
    if (line.rfind("--- ", 0) == 0) {
      if (hasCurrentFile) flush();
      hasCurrentFile = true;
      currentOldPath = line.substr(4);
      currentLines.push_back(line);
      continue;
    }
    if (line.rfind("+++ ", 0) == 0) {
      hasCurrentFile = true;
      currentNewPath = line.substr(4);
      currentLines.push_back(line);
      continue;
    }
    if (hasCurrentFile) {
      currentLines.push_back(line);
    }
  }
  flush();

  if (targets.empty()) {
    throw std::runtime_error(
        "Patch input must be a unified diff with ---/+++ file headers. Edit is patch-only now.");
  }
  for (const auto &target : targets) {
    if (target.path.empty()) {
      throw std::runtime_error("Patch input is missing a resolvable target path in headers.");
    }
  }
  return targets;
}

PreparedFileMutation prepareRangeMutation(const std::string &path,
                                          const std::vector<FileEditOperationInput> &operations,
                                          shared::ToolContext &ctx) {
  PreparedFileMutation prepared;
  prepared.requestPath = path;
  prepared.absolutePath = ctx.agent.getEnvironment()->getWorkspace().resolvePath(path);
  prepared.mode = "range";

  if (path.empty()) {
    throw std::runtime_error("EditRange requires path.");
  }
  if (!ctx.host.exists(prepared.absolutePath)) {
    throw std::runtime_error("EditRange requires an existing file.");
  }

  ctx.agent.getPermissions()->validatePathAccess(prepared.absolutePath, AccessMode::WRITE);
  prepared.beforeLsp = std::make_unique<rapidjson::Document>(collectFileLspDiagnostics(prepared.absolutePath, ctx));
  auto data = ctx.host.readFile(prepared.absolutePath);
  std::string content(data.begin(), data.end());
  FileBuffer buffer = splitFileContent(content);
  auto norm = normalizeRangeEdits(operations, buffer.lines, &prepared.sanitation);
  if (!norm.errors.empty()) {
    auto doc = buildRangeFailureDoc(path, operations, norm, prepared.sanitation);
    rapidjson::StringBuffer sb;
    rapidjson::Writer<rapidjson::StringBuffer> writer(sb);
    doc.Accept(writer);
    throw std::runtime_error(sb.GetString());
  }
  prepared.normalizedEdits = norm.normalized;
  for (const auto &edit : prepared.normalizedEdits) {
    prepared.removedLines += edit.endIndex - edit.startIndex;
    prepared.addedLines += static_cast<int>(edit.newLines.size());
    if (edit.relocated) prepared.relocatedAnchors++;
  }
  for (auto it = prepared.normalizedEdits.rbegin(); it != prepared.normalizedEdits.rend(); ++it) {
    buffer.lines.erase(buffer.lines.begin() + it->startIndex,
                       buffer.lines.begin() + it->endIndex);
    buffer.lines.insert(buffer.lines.begin() + it->startIndex,
                        it->newLines.begin(), it->newLines.end());
  }
  prepared.updatedContent = joinFileContent(buffer);
  return prepared;
}

PreparedFileMutation preparePatchMutation(const PatchTarget &target,
                                          shared::ToolContext &ctx) {
  try {
    auto prepared = prepareRangeMutation(target.path, target.edits, ctx);
    prepared.mode = "patch";
    return prepared;
  } catch (const std::runtime_error &e) {
    rapidjson::Document parsed;
    parsed.Parse(e.what());
    if (!parsed.HasParseError() && parsed.IsObject() &&
        parsed.HasMember("resolved_mode")) {
      parsed["resolved_mode"].SetString("patch", parsed.GetAllocator());
      if (parsed.HasMember("error") && parsed["error"].IsString()) {
        parsed["error"].SetString(
            "Patch hunk context not found or no longer matches the target file.",
            parsed.GetAllocator());
      }
      if (parsed.HasMember("suggestion")) {
        parsed["suggestion"].SetString(
            "Re-read the target file and regenerate the patch from current text.",
            parsed.GetAllocator());
      } else {
        parsed.AddMember(
            "suggestion",
            rapidjson::Value(
                "Re-read the target file and regenerate the patch from current text.",
                parsed.GetAllocator())
                .Move(),
            parsed.GetAllocator());
      }
      rapidjson::StringBuffer sb;
      rapidjson::Writer<rapidjson::StringBuffer> writer(sb);
      parsed.Accept(writer);
      throw std::runtime_error(sb.GetString());
    }
    throw;
  }
}

PreparedFileMutation prepareWriteMutation(const FileWriteInput &input,
                                          shared::ToolContext &ctx) {
  if (input.path.empty()) {
    throw std::runtime_error("EditWrite requires path.");
  }
  PreparedFileMutation prepared;
  prepared.requestPath = input.path;
  prepared.absolutePath = ctx.agent.getEnvironment()->getWorkspace().resolvePath(input.path);
  prepared.mode = "write";
  ctx.agent.getPermissions()->validatePathAccess(prepared.absolutePath, AccessMode::WRITE);
  const bool exists = ctx.host.exists(prepared.absolutePath);
  if (exists) {
    prepared.beforeLsp = std::make_unique<rapidjson::Document>(collectFileLspDiagnostics(prepared.absolutePath, ctx));
    auto data = ctx.host.readFile(prepared.absolutePath);
    std::string previous(data.begin(), data.end());
    auto oldLines = splitLinesSimple(previous);
    auto newLines = splitLinesSimple(input.content);
    prepared.normalizedEdits.push_back({"overwrite_file_content", 0,
                                        static_cast<int>(oldLines.size()), newLines,
                                        "overwrite file", false, oldLines});
    prepared.removedLines = static_cast<int>(oldLines.size());
    prepared.addedLines = static_cast<int>(newLines.size());
  } else {
    auto newLines = splitLinesSimple(input.content);
    prepared.normalizedEdits.push_back({"create_file", 0, 0, newLines,
                                        "create file", false, {}});
    prepared.addedLines = static_cast<int>(newLines.size());
  }
  prepared.updatedContent = input.content;
  return prepared;
}

PreparedFileMutation prepareReplaceMutation(const FileReplaceInput &input,
                                            shared::ToolContext &ctx) {
  if (input.path.empty()) {
    throw std::runtime_error("EditReplace requires path.");
  }
  PreparedFileMutation prepared;
  prepared.requestPath = input.path;
  prepared.absolutePath = ctx.agent.getEnvironment()->getWorkspace().resolvePath(input.path);
  prepared.mode = "replace";
  if (!ctx.host.exists(prepared.absolutePath)) {
    throw std::runtime_error("EditReplace requires an existing file.");
  }
  ctx.agent.getPermissions()->validatePathAccess(prepared.absolutePath, AccessMode::WRITE);
  prepared.beforeLsp = std::make_unique<rapidjson::Document>(collectFileLspDiagnostics(prepared.absolutePath, ctx));
  auto data = ctx.host.readFile(prepared.absolutePath);
  std::string current(data.begin(), data.end());
  std::string newContent = current;

  for (const auto &replacement : input.replacements) {
    FileEditOperationInput edit;
    edit.op = "search_replace";
    edit.old_string = replacement.old_string;
    edit.new_string = replacement.new_string;
    edit.has_old_string = true;
    edit.has_new_string = true;
    edit.replace_all = replacement.replace_all;
    validateReplacementOperation(edit);

    const std::string beforeEdit = newContent;
    size_t pos = 0;
    int localReplacements = 0;
    if (replacement.replace_all) {
      while ((pos = newContent.find(replacement.old_string, pos)) != std::string::npos) {
        newContent.replace(pos, replacement.old_string.size(), replacement.new_string);
        pos += replacement.new_string.size();
        localReplacements++;
      }
    } else {
      pos = newContent.find(replacement.old_string);
      if (pos != std::string::npos) {
        newContent.replace(pos, replacement.old_string.size(), replacement.new_string);
        localReplacements++;
      }
    }
    if (localReplacements == 0) {
      throw std::runtime_error("Could not find \"" + replacement.old_string +
                               "\" in the file. Ensure the file content matches.");
    }
    prepared.replacements += localReplacements;
    if (auto preview = buildSearchReplacePreview(beforeEdit, newContent, edit)) {
      prepared.addedLines += static_cast<int>(preview->newLines.size());
      prepared.removedLines += static_cast<int>(preview->oldLines.size());
      prepared.normalizedEdits.push_back(std::move(*preview));
    }
  }
  prepared.updatedContent = newContent;
  return prepared;
}

void applyPreparedMutation(const PreparedFileMutation &prepared,
                           shared::ToolContext &ctx) {
  ctx.host.writeFile(prepared.absolutePath,
                     std::vector<uint8_t>(prepared.updatedContent.begin(),
                                          prepared.updatedContent.end()));
  ctx.agent.getEnvironment()->getWorkspace().recordFileEdit(prepared.absolutePath);
}

rapidjson::Document buildPreparedMutationDoc(const PreparedFileMutation &prepared,
                                             shared::ToolContext &ctx,
                                             const char *shape,
                                             bool transactional) {
  rapidjson::Document doc;
  doc.SetObject();
  auto &alloc = doc.GetAllocator();
  doc.AddMember("path", rapidjson::Value(prepared.requestPath.c_str(), alloc).Move(), alloc);
  doc.AddMember("resolved_mode", rapidjson::Value(prepared.mode.c_str(), alloc).Move(), alloc);
  doc.AddMember("shape", rapidjson::Value(shape, alloc).Move(), alloc);
  doc.AddMember("transactional", transactional, alloc);
  doc.AddMember("applied_edits", static_cast<uint32_t>(prepared.normalizedEdits.size()), alloc);
  doc.AddMember("added_lines", prepared.addedLines, alloc);
  doc.AddMember("removed_lines", prepared.removedLines, alloc);
  if (prepared.mode == "replace") {
    doc.AddMember("replacements", prepared.replacements, alloc);
  }
  if (prepared.mode == "range" || prepared.mode == "patch") {
    doc.AddMember("relocated_anchors", prepared.relocatedAnchors, alloc);
    addSanitationMember(doc, prepared.sanitation, alloc);
  }
  const std::string diffPreview = buildDiffPreview(prepared.normalizedEdits);
  doc.AddMember("diff_preview", rapidjson::Value(diffPreview.c_str(), alloc).Move(), alloc);
  rapidjson::Value operations(rapidjson::kArrayType);
  for (const auto &edit : prepared.normalizedEdits) {
    operations.PushBack(buildOperationResult(edit, alloc), alloc);
  }
  doc.AddMember("operations", operations, alloc);
  doc.AddMember("watch_state", rapidjson::Value("refreshed", alloc).Move(), alloc);
  auto afterLsp = collectFileLspDiagnostics(prepared.absolutePath, ctx);
  attachFileEditLspSummary(doc, prepared.absolutePath, prepared.beforeLsp.get(), &afterLsp);
  return doc;
}

rapidjson::Document buildValidationDoc(const char *mode, const char *shape,
                                       size_t fileCount,
                                       const std::vector<std::string> &paths) {
  rapidjson::Document doc;
  doc.SetObject();
  auto &alloc = doc.GetAllocator();
  doc.AddMember("resolved_mode", rapidjson::Value(mode, alloc).Move(), alloc);
  doc.AddMember("shape", rapidjson::Value(shape, alloc).Move(), alloc);
  doc.AddMember("validate_only", true, alloc);
  doc.AddMember("valid", true, alloc);
  doc.AddMember("transactional", std::string_view(mode) == "patch", alloc);
  doc.AddMember("file_count", static_cast<uint32_t>(fileCount), alloc);
  rapidjson::Value files(rapidjson::kArrayType);
  for (const auto &path : paths) {
    files.PushBack(rapidjson::Value(path.c_str(), alloc).Move(), alloc);
  }
  doc.AddMember("files", files, alloc);
  doc.AddMember("would_change", cloneJsonValue(files, alloc), alloc);
  return doc;
}

} // namespace

std::shared_ptr<shared::JSONSchema> FileEditTool::getSchema() const {
  return zObject({
      {"patch",
       zString()->describe(
           "Unified diff (unidiff) patch text with ---/+++ headers. Supports one or more files.\n\n"
           "USAGE:\n"
           "- Provide a standard unified diff with file headers and one or more hunks.\n"
           "- Multi-file patches are allowed; they will be applied transactionally (all succeed or none).\n"
           "- Generate the patch from the latest on-disk content (use Files.Read first) to avoid hunk mismatch.\n\n"
           "COMMON PITFALLS:\n"
           "- Missing or malformed ---/+++ headers.\n"
           "- Context doesn't match because the file changed since you generated the diff.\n"
           "- Attempting to use this tool for whole-file writes (use EditWrite instead).")},
      {"validate_only",
       zBoolean()
           ->describe(
               "When true, validate the patch (shape + target extraction + permissions) without writing any files.\n\n"
               "Use this to preflight risky patches or debug application failures.\n"
               "Returns a structured preview of what would change.")
           ->setOptional()}
  })->required({"patch"});
}

FilePatchInput FileEditTool::transform(const rapidjson::Value &json) {
  FilePatchInput input;
  if (json.HasMember("patch") && json["patch"].IsString()) {
    input.patch = json["patch"].GetString();
  }
  if (json.HasMember("validate_only") && json["validate_only"].IsBool()) {
    input.validate_only = json["validate_only"].GetBool();
  }
  return input;
}

shared::ToolMetadata FileEditTool::getMetadata() const {
  return {
      "Edit",
      R"(Apply unified diffs transactionally (patch-first editing).

USAGE GUIDANCE:
- Use this tool when you have an exact unified diff (---/+++ headers) to apply.
- Prefer generating the patch from the exact file content you just read (via Files.Read) to avoid mismatched context.
- For multi-file edits, include multiple file sections in one patch; this tool applies them transactionally.
- If you only need a whole-file overwrite, use EditWrite.
- If you need deterministic literal string replacement, use EditReplace.
- If you need anchored line-range operations (insert/replace around anchors), use EditRange.

VALIDATION MODE:
- Set validate_only=true to check patch shape/targets without writing.
- A successful validation returns what WOULD change; it does not modify files.

FAILURE MODES / RECOVERY:
- If the patch fails to apply, re-read the target file and regenerate the patch against current contents.
- Keep hunks small and context-rich; large hunks are brittle.
)",
      ToolScope::FilesystemWrite};
}

shared::ToolResult FileEditTool::execute(const FilePatchInput &input,
                                         shared::ToolContext &ctx) {
  try {
    const auto targets = splitUnifiedPatchTargets(input.patch);
    std::vector<PreparedFileMutation> prepared;
    std::vector<std::string> paths;
    prepared.reserve(targets.size());
    paths.reserve(targets.size());
    for (const auto &target : targets) {
      paths.push_back(target.path);
      prepared.push_back(preparePatchMutation(target, ctx));
    }
    if (input.validate_only) {
      return shared::ToolResult::ok(buildValidationDoc("patch", targets.size() > 1 ? "multi_file" : "single_file",
                                                       targets.size(), paths));
    }
    for (const auto &mutation : prepared) {
      applyPreparedMutation(mutation, ctx);
    }

    if (prepared.size() == 1) {
      return shared::ToolResult::ok(buildPreparedMutationDoc(prepared.front(), ctx, "single_file", true));
    }
    rapidjson::Document aggregate;
    aggregate.SetObject();
    auto &alloc = aggregate.GetAllocator();
    aggregate.AddMember("resolved_mode", rapidjson::Value("patch", alloc).Move(), alloc);
    aggregate.AddMember("shape", rapidjson::Value("multi_file", alloc).Move(), alloc);
    aggregate.AddMember("transactional", true, alloc);
    aggregate.AddMember("file_count", static_cast<uint32_t>(prepared.size()), alloc);
    rapidjson::Value files(rapidjson::kArrayType);
    rapidjson::Value editedPaths(rapidjson::kArrayType);
    int added = 0;
    int removed = 0;
    uint32_t applied = 0;
    for (const auto &mutation : prepared) {
      auto doc = buildPreparedMutationDoc(mutation, ctx, "single_file", true);
      files.PushBack(cloneJsonValue(doc, alloc), alloc);
      editedPaths.PushBack(rapidjson::Value(mutation.requestPath.c_str(), alloc).Move(), alloc);
      added += mutation.addedLines;
      removed += mutation.removedLines;
      applied += static_cast<uint32_t>(mutation.normalizedEdits.size());
    }
    aggregate.AddMember("files", files, alloc);
    aggregate.AddMember("edited_files", editedPaths, alloc);
    aggregate.AddMember("applied_edits", applied, alloc);
    aggregate.AddMember("added_lines", added, alloc);
    aggregate.AddMember("removed_lines", removed, alloc);
    aggregate.AddMember("watch_state", rapidjson::Value("refreshed", alloc).Move(), alloc);
    return shared::ToolResult::ok(aggregate);
  } catch (const std::exception &e) {
    return shared::ToolResult::fail(e.what());
  }
}

std::shared_ptr<shared::JSONSchema> FileWriteTool::getSchema() const {
  return zObject({
      {"path",
       zString()->describe(
           "Absolute or workspace-relative path of the file to create/overwrite.\n\n"
           "Usage: Prefer workspace-relative paths. The path is resolved against the workspace root.\n"
           "Permissions: requires filesystem WRITE access to the resolved path.")},
      {"content",
       zString()->describe(
           "The complete new contents of the file.\n\n"
           "WARNING: This replaces the entire file. Use Edit or EditRange for small edits.")},
      {"validate_only",
       zBoolean()
           ->describe(
               "When true, validate the request (path resolution + permissions) without writing the file.\n\n"
               "Use this to preflight risky overwrites.")
           ->setOptional()}
  })->required({"path", "content"});
}

FileWriteInput FileWriteTool::transform(const rapidjson::Value &json) {
  FileWriteInput input;
  if (json.HasMember("path") && json["path"].IsString()) input.path = json["path"].GetString();
  if (json.HasMember("content") && json["content"].IsString()) input.content = json["content"].GetString();
  if (json.HasMember("validate_only") && json["validate_only"].IsBool()) input.validate_only = json["validate_only"].GetBool();
  return input;
}

shared::ToolMetadata FileWriteTool::getMetadata() const {
  return {"EditWrite",
          R"(Write complete file contents to create or overwrite a single file.

USAGE GUIDANCE:
- Use this tool to create a new file or replace an entire file's contents.
- Do NOT use this tool for small targeted edits; prefer Edit (unified diff) or EditRange (anchored) to minimize accidental overwrites.
- Always read the existing file first (Files.Read) unless you are intentionally creating a brand-new file.

VALIDATION MODE:
- validate_only=true checks that the path is writable and that the request is well-formed, without writing.

SAFETY NOTES:
- This overwrites the entire file. If you pass incomplete content you will lose data.
)",
          ToolScope::FilesystemWrite};
}

shared::ToolResult FileWriteTool::execute(const FileWriteInput &input,
                                          shared::ToolContext &ctx) {
  try {
    auto prepared = prepareWriteMutation(input, ctx);
    if (input.validate_only) {
      return shared::ToolResult::ok(buildValidationDoc("write", "single_file", 1, {input.path}));
    }
    applyPreparedMutation(prepared, ctx);
    return shared::ToolResult::ok(buildPreparedMutationDoc(prepared, ctx, "single_file", false));
  } catch (const std::exception &e) {
    return shared::ToolResult::fail(e.what());
  }
}

std::shared_ptr<shared::JSONSchema> FileReplaceTool::getSchema() const {
  auto replacementSchema = zObject({
      {"old_string", zString()->describe("Exact literal text to find.")},
      {"new_string", zString()->describe("Replacement text. May be empty.")},
      {"replace_all", zBoolean()->describe("Replace every match instead of only the first.")->setOptional()}
  })->required({"old_string", "new_string"});
  return zObject({
      {"path", zString()->describe("Absolute or relative path to the file.")},
      {"replacements", zArray(replacementSchema)->describe("One or more literal replacements to apply in order.")},
      {"validate_only", zBoolean()->describe("When true, validate the request without writing.")->setOptional()}
  })->required({"path", "replacements"});
}

FileReplaceInput FileReplaceTool::transform(const rapidjson::Value &json) {
  FileReplaceInput input;
  if (json.HasMember("path") && json["path"].IsString()) input.path = json["path"].GetString();
  if (json.HasMember("validate_only") && json["validate_only"].IsBool()) input.validate_only = json["validate_only"].GetBool();
  if (json.HasMember("replacements") && json["replacements"].IsArray()) {
    for (const auto &value : json["replacements"].GetArray()) {
      if (!value.IsObject()) continue;
      FileReplaceInput::Replacement replacement;
      if (value.HasMember("old_string") && value["old_string"].IsString()) replacement.old_string = value["old_string"].GetString();
      if (value.HasMember("new_string") && value["new_string"].IsString()) replacement.new_string = value["new_string"].GetString();
      if (value.HasMember("replace_all") && value["replace_all"].IsBool()) replacement.replace_all = value["replace_all"].GetBool();
      input.replacements.push_back(std::move(replacement));
    }
  }
  return input;
}

shared::ToolMetadata FileReplaceTool::getMetadata() const {
  return {"EditReplace", "Apply literal replacements to a single existing file.", ToolScope::FilesystemWrite};
}

shared::ToolResult FileReplaceTool::execute(const FileReplaceInput &input,
                                            shared::ToolContext &ctx) {
  try {
    auto prepared = prepareReplaceMutation(input, ctx);
    if (input.validate_only) {
      return shared::ToolResult::ok(buildValidationDoc("replace", "single_file", 1, {input.path}));
    }
    applyPreparedMutation(prepared, ctx);
    return shared::ToolResult::ok(buildPreparedMutationDoc(prepared, ctx, "single_file", false));
  } catch (const std::exception &e) {
    return shared::ToolResult::fail(e.what());
  }
}

std::shared_ptr<shared::JSONSchema> FileRangeTool::getSchema() const {
  auto opSchema = zObject({
      {"op", zEnum({"replace_range", "insert_after", "insert_before", "delete_range"})
                 ->describe("Range operation type.")},
      {"start_anchor", zString()->describe("Start line anchor for replace/delete.")->setOptional()},
      {"end_anchor", zString()->describe("End line anchor for replace/delete.")->setOptional()},
      {"anchor", zString()->describe("Single anchor for insert operations.")->setOptional()},
      {"new_lines", zArray(zString())->describe("Replacement or inserted lines without trailing newlines.")->setOptional()}
  })->required({"op"});
  return zObject({
      {"path", zString()->describe("Absolute or relative path to the file.")},
      {"operations", zArray(opSchema)->describe("One or more line-range operations for a single existing file.")},
      {"validate_only", zBoolean()->describe("When true, validate the request without writing.")->setOptional()}
  })->required({"path", "operations"});
}

FileRangeInput FileRangeTool::transform(const rapidjson::Value &json) {
  FileRangeInput input;
  if (json.HasMember("path") && json["path"].IsString()) input.path = json["path"].GetString();
  if (json.HasMember("validate_only") && json["validate_only"].IsBool()) input.validate_only = json["validate_only"].GetBool();
  if (json.HasMember("operations") && json["operations"].IsArray()) {
    for (const auto &value : json["operations"].GetArray()) {
      if (!value.IsObject()) continue;
      FileEditOperationInput edit;
      if (value.HasMember("op") && value["op"].IsString()) edit.op = value["op"].GetString();
      if (value.HasMember("start_anchor") && value["start_anchor"].IsString()) edit.start_anchor = value["start_anchor"].GetString();
      if (value.HasMember("end_anchor") && value["end_anchor"].IsString()) edit.end_anchor = value["end_anchor"].GetString();
      if (value.HasMember("anchor") && value["anchor"].IsString()) edit.anchor = value["anchor"].GetString();
      edit.new_lines = readStringArray(value, "new_lines");
      input.operations.push_back(std::move(edit));
    }
  }
  return input;
}

shared::ToolMetadata FileRangeTool::getMetadata() const {
  return {"EditRange", "Apply anchored line-range operations to a single existing file.", ToolScope::FilesystemWrite};
}

shared::ToolResult FileRangeTool::execute(const FileRangeInput &input,
                                          shared::ToolContext &ctx) {
  try {
    auto prepared = prepareRangeMutation(input.path, input.operations, ctx);
    if (input.validate_only) {
      return shared::ToolResult::ok(buildValidationDoc("range", "single_file", 1, {input.path}));
    }
    applyPreparedMutation(prepared, ctx);
    return shared::ToolResult::ok(buildPreparedMutationDoc(prepared, ctx, "single_file", false));
  } catch (const std::exception &e) {
    return shared::ToolResult::fail(e.what());
  }
}

} // namespace firmius::core

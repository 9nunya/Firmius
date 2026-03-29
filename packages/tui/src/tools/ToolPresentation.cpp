#include "tools/ArtifactToolPresentation.hpp"
#include "tools/FileToolPresentation.hpp"
#include "tools/ProcessToolPresentation.hpp"
#include "tools/SubagentToolPresentation.hpp"
#include "tools/ToolPresentation.hpp"
#include "tools/WorkToolPresentation.hpp"
#include "tools/PythonToolPresentation.hpp"

#include "utils/ErrorCleaner.hpp"
#include "utils/ToolSummaries.hpp"
#include <rapidjson/document.h>
#include <optional>
#include <sstream>

namespace firmius::tui {

using firmius::shared::SummarizeToolCall;
using firmius::shared::TailLines;
using firmius::shared::ToolCallView;
using firmius::shared::ToolPhase;

namespace {

std::optional<ToolPresentation>
TryBuildSpecializedPresentation(const ToolCallView &view,
                                const NormalizedProcessState *process_state,
                                const NormalizedSubagentState *subagent_state);
ToolPresentationLifecycle DeriveLifecycle(const ToolCallView &view);

bool IsMatch(const std::string &actual, const std::string &expected) {
  if (actual.empty() || expected.empty()) {
    return false;
  }
  return actual.find(expected) != std::string::npos;
}

bool ParseObject(const std::string &json, rapidjson::Document &doc) {
  doc.Parse(json.c_str());
  return !doc.HasParseError() && doc.IsObject();
}

std::string StringMember(const rapidjson::Value &value, const char *key) {
  if (value.IsObject() && value.HasMember(key) && value[key].IsString()) {
    return value[key].GetString();
  }
  return "";
}

std::string BuildSearchTitle(const ToolCallView &view, const std::string &pattern,
                             int match_count) {
  if (view.phase == ToolPhase::Preparing || view.phase == ToolPhase::Called ||
      view.phase == ToolPhase::BackgroundRunning) {
    return SummarizeToolCall(view.name, view.args, view.phase);
  }

  const bool is_grep = IsMatch(view.name, "grep");
  std::string title = is_grep ? "grep" : "glob";
  if (!pattern.empty()) {
    title += " \"" + pattern + "\"";
  }
  if (match_count >= 0) {
    title += " (" + std::to_string(match_count) + " matches)";
  }
  return title;
}

std::string FormatSearchResultRow(const rapidjson::Value &item) {
  if (item.IsString()) {
    return item.GetString();
  }
  if (!item.IsObject()) {
    return "";
  }

  std::string path = StringMember(item, "path");
  std::string type = StringMember(item, "type");
  std::string content = StringMember(item, "line");
  if (content.empty()) {
    content = StringMember(item, "content");
  }
  if (content.empty()) {
    content = StringMember(item, "text");
  }

  int line_number = -1;
  if (item.HasMember("line_number") && item["line_number"].IsInt()) {
    line_number = item["line_number"].GetInt();
  }
  if (line_number < 0 && item.HasMember("line") && item["line"].IsInt()) {
    line_number = item["line"].GetInt();
  }

  std::string row = path;
  if (type == "directory") {
    row = "[dir] " + path;
  } else if (type == "file") {
    row = "[file] " + path;
  }
  if (line_number >= 0 && !row.empty()) {
    row += ":" + std::to_string(line_number);
  }
  if (!content.empty()) {
    if (!row.empty()) {
      row += " - ";
    }
    row += content;
  }
  return row;
}

ToolPresentation BuildSearchPresentation(const ToolCallView &view) {
  ToolPresentation presentation;
  presentation.lifecycle = DeriveLifecycle(view);
  presentation.layout = ToolPresentationLayoutKind::ResultsList;
  presentation.density = ToolPresentationDensity::DetailHeavy;

  rapidjson::Document args_doc;
  std::string pattern;
  if (ParseObject(view.args, args_doc)) {
    pattern = StringMember(args_doc, "pattern");
  }

  rapidjson::Document result_doc;
  bool has_result_array = false;
  int match_count = -1;
  if (!view.result.empty()) {
    result_doc.Parse(view.result.c_str());
    has_result_array = !result_doc.HasParseError() && result_doc.IsArray();
    if (has_result_array) {
      match_count = static_cast<int>(result_doc.Size());
    }
  }

  presentation.title = BuildSearchTitle(view, pattern, match_count);
  presentation.subtitle = view.name;
  presentation.compact_summary = presentation.title;
  presentation.facts.push_back({"Tool", view.name});
  if (!pattern.empty()) {
    presentation.facts.push_back({"Pattern", pattern});
  }
  if (match_count >= 0) {
    presentation.facts.push_back({"Matches", std::to_string(match_count)});
  }

  if (presentation.lifecycle == ToolPresentationLifecycle::Error) {
    presentation.error_text = firmius::shared::ErrorCleaner::clean(view.result);
    if (presentation.title.find("failed") == std::string::npos) {
      presentation.title += " failed";
    }
    return presentation;
  }

  if (presentation.lifecycle == ToolPresentationLifecycle::Preparing ||
      presentation.lifecycle == ToolPresentationLifecycle::Running ||
      !has_result_array) {
    return presentation;
  }

  const int kMaxRows = view.show_result ? static_cast<int>(result_doc.Size()) : 20;
  ToolPresentationSection section;
  section.title = "Matches";
  for (rapidjson::SizeType i = 0;
       i < result_doc.Size() && i < static_cast<rapidjson::SizeType>(kMaxRows);
       ++i) {
    std::string row = FormatSearchResultRow(result_doc[i]);
    if (row.empty()) {
      continue;
    }
    section.lines.push_back(row);
    presentation.body_lines.push_back(row);
  }
  if (!section.lines.empty()) {
    presentation.sections.push_back(std::move(section));
    presentation.expandable = true;
    presentation.expanded = view.show_result;
  }
  if (result_doc.Size() > static_cast<rapidjson::SizeType>(kMaxRows)) {
    ToolPresentationNotice notice;
    notice.kind = ToolPresentationNoticeKind::Info;
    notice.text = "Show to open full match list";
    presentation.notices.push_back(std::move(notice));
  }
  return presentation;
}

std::optional<ToolPresentation>
TryBuildSpecializedPresentation(const ToolCallView &view,
                                const NormalizedProcessState *process_state,
                                const NormalizedSubagentState *subagent_state) {
  if (IsMatch(view.name, "python_execute")) {
    return BuildPythonToolPresentation(view, process_state);
  }
  if (IsProcessFamilyTool(view.name)) {
    return BuildProcessToolPresentation(view, process_state);
  }
  if (IsSubagentFamilyTool(view.name)) {
    return BuildSubagentToolPresentation(view, subagent_state);
  }
  if (IsArtifactFamilyTool(view.name)) {
    return BuildArtifactToolPresentation(view);
  }
  if (IsWorkFamilyTool(view.name)) {
    return BuildWorkToolPresentation(view);
  }
  if (IsFileFamilyTool(view.name)) {
    return BuildFileToolPresentation(view);
  }
  if (IsMatch(view.name, "grep") || IsMatch(view.name, "glob")) {
    return BuildSearchPresentation(view);
  }
  // Foundation seam for future per-tool-family presenters.
  return std::nullopt;
}

ToolPresentationLifecycle DeriveLifecycle(const ToolCallView &view) {
  if (view.phase == ToolPhase::Preparing) {
    return ToolPresentationLifecycle::Preparing;
  }
  if (view.phase == ToolPhase::Called ||
      view.phase == ToolPhase::BackgroundRunning) {
    return ToolPresentationLifecycle::Running;
  }
  if (view.phase == ToolPhase::Error ||
      (view.phase == ToolPhase::Finished && !view.success)) {
    return ToolPresentationLifecycle::Error;
  }
  return ToolPresentationLifecycle::Success;
}

int CountLines(const std::string &text) {
  std::istringstream stream(text);
  int count = 0;
  std::string line;
  while (std::getline(stream, line)) {
    count++;
  }
  return count;
}

ToolPresentation BuildGenericPresentation(const ToolCallView &view) {
  ToolPresentation presentation;
  presentation.lifecycle = DeriveLifecycle(view);
  presentation.layout = ToolPresentationLayoutKind::CompactFactCard;
  presentation.density = ToolPresentationDensity::DetailHeavy;
  presentation.title = SummarizeToolCall(view.name, view.args, view.phase);
  presentation.subtitle = view.name.empty() ? "tool" : view.name;
  presentation.compact_summary = presentation.title;
  presentation.facts.push_back(
      {"Tool", view.name.empty() ? std::string("(unnamed)") : view.name});

  if (presentation.lifecycle == ToolPresentationLifecycle::Error) {
    presentation.error_text = firmius::shared::ErrorCleaner::clean(view.result);
    presentation.title += " failed";
    return presentation;
  }

  if (presentation.lifecycle == ToolPresentationLifecycle::Preparing ||
      presentation.lifecycle == ToolPresentationLifecycle::Running) {
    return presentation;
  }

  if (view.result.empty()) {
    return presentation;
  }

  constexpr int kMaxPreviewLines = 5;
  const int total_line_count = CountLines(view.result);
  presentation.expandable = true;
  presentation.expanded = view.show_result;
  presentation.facts.push_back({"Output lines", std::to_string(total_line_count)});

  ToolPresentationSection section;
  section.title = "Result preview";
  section.lines = TailLines(view.result, kMaxPreviewLines);
  presentation.body_lines = section.lines;
  presentation.sections.push_back(std::move(section));

  if (total_line_count > kMaxPreviewLines) {
    ToolPresentationNotice notice;
    notice.kind = ToolPresentationNoticeKind::Info;
    notice.text = "Showing last " + std::to_string(kMaxPreviewLines) +
                  " of " + std::to_string(total_line_count) + " lines";
    presentation.notices.push_back(std::move(notice));
  }

  return presentation;
}

} // namespace

ToolPresentation BuildToolPresentation(const ToolCallView &view,
                                       const NormalizedProcessState *process_state,
                                       const NormalizedSubagentState *subagent_state) {
  if (auto specialized =
          TryBuildSpecializedPresentation(view, process_state, subagent_state)) {
    return *specialized;
  }
  return BuildGenericPresentation(view);
}

} // namespace firmius::tui

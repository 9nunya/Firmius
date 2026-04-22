#include "tools/SearchToolPresentation.hpp"

#include "utils/ErrorCleaner.hpp"
#include "utils/ToolSummaries.hpp"

#include <rapidjson/document.h>

namespace firmius::tui {
namespace {

using firmius::shared::SummarizeToolCall;
using firmius::shared::ToolCallView;
using firmius::shared::ToolPhase;

bool IsMatch(const std::string &actual, const std::string &expected) {
  if (actual.empty() || expected.empty()) {
    return false;
  }
  return actual.find(expected) != std::string::npos;
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

  rapidjson::Document args_doc;
  const bool has_args = ParseObject(view.args, args_doc);
  const std::string action = has_args ? StringMember(args_doc, "action") : "";
  const bool is_grep = IsMatch(view.name, "grep") || action == "Grep";
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

} // namespace

bool IsSearchFamilyTool(const std::string &tool_name) {
  return tool_name == "Search" || IsMatch(tool_name, "grep") ||
         IsMatch(tool_name, "glob");
}

ToolPresentation BuildSearchToolPresentation(const ToolCallView &view) {
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

} // namespace firmius::tui

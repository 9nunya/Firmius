#include "tools/WebSearchToolPresentation.hpp"

#include "utils/ErrorCleaner.hpp"
#include "utils/Icons.hpp"

#include <rapidjson/document.h>
#include <sstream>

namespace firmius::tui {

namespace {

using firmius::shared::ToolCallView;
using firmius::shared::ToolPhase;

bool IsMatch(const std::string &actual, const std::string &expected) {
  return !actual.empty() && !expected.empty() &&
         actual.find(expected) != std::string::npos;
}

ToolPresentationLifecycle LifecycleFromPhase(const ToolCallView &view) {
  if (view.phase == ToolPhase::Preparing) {
    return ToolPresentationLifecycle::Preparing;
  }
  if (view.phase == ToolPhase::Called || view.phase == ToolPhase::BackgroundRunning) {
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

bool ParseArray(const std::string &json, rapidjson::Document &doc) {
  doc.Parse(json.c_str());
  return !doc.HasParseError() && doc.IsArray();
}

std::string StringMember(const rapidjson::Value &value, const char *key) {
  if (value.IsObject() && value.HasMember(key) && value[key].IsString()) {
    return value[key].GetString();
  }
  return "";
}

void ApplyError(ToolPresentation &presentation, const ToolCallView &view,
                const std::string &title) {
  presentation.lifecycle = ToolPresentationLifecycle::Error;
  presentation.title = title;
  presentation.subtitle = view.name;
  presentation.custom_icon = firmius::shared::ICON_SEARCH;
  presentation.error_text = firmius::shared::ErrorCleaner::clean(view.result);
}

} // namespace

bool IsWebSearchFamilyTool(const std::string &tool_name) {
  return tool_name == "Web" || IsMatch(tool_name, "web_search") ||
         IsMatch(tool_name, "web_fetch");
}

ToolPresentation BuildWebSearchToolPresentation(const ToolCallView &view) {
  ToolPresentation presentation;
  presentation.lifecycle = LifecycleFromPhase(view);
  presentation.layout = ToolPresentationLayoutKind::InlineStatusRow;
  presentation.density = ToolPresentationDensity::OneLineSummary;
  presentation.subtitle = view.name;
  presentation.custom_icon = firmius::shared::ICON_SEARCH;

  rapidjson::Document args_doc;
  const bool has_args = ParseObject(view.args, args_doc);
  const std::string query = has_args ? StringMember(args_doc, "query") : "";

  presentation.title = query.empty() ? "..." : query;

  if (presentation.lifecycle == ToolPresentationLifecycle::Error) {
    ApplyError(presentation, view, "Web search failed");
    return presentation;
  }

  if (presentation.lifecycle != ToolPresentationLifecycle::Success) {
    return presentation;
  }

  rapidjson::Document result_doc;
  if (!ParseObject(view.result, result_doc) && !ParseArray(view.result, result_doc)) {
      return presentation;
  }

  const rapidjson::Value *results_array = nullptr;
  if (result_doc.IsArray()) {
    results_array = &result_doc;
  } else if (result_doc.IsObject()) {
    if (result_doc.HasMember("results") && result_doc["results"].IsArray()) {
      results_array = &result_doc["results"];
    } else if (result_doc.HasMember("sources") && result_doc["sources"].IsArray()) {
      results_array = &result_doc["sources"];
    }
  }

  const bool has_result = result_doc.IsObject();
  const std::string content = has_result ? StringMember(result_doc, "content") : "";
  const std::string provider_name = has_result ? StringMember(result_doc, "provider") : "";

  if (results_array) {
    presentation.footer_badges.push_back(std::to_string(results_array->Size()) + " results");
  }

  return presentation;
}

ToolPresentation BuildWebFetchToolPresentation(const ToolCallView &view) {
  ToolPresentation presentation;
  presentation.lifecycle = LifecycleFromPhase(view);
  presentation.layout = ToolPresentationLayoutKind::BodyFirstPreview;
  presentation.density = ToolPresentationDensity::DetailHeavy;
  presentation.subtitle = view.name;
  presentation.custom_icon = firmius::shared::ICON_SEARCH;

  rapidjson::Document args_doc;
  const bool has_args = ParseObject(view.args, args_doc);
  const std::string url = has_args ? StringMember(args_doc, "url") : "";

  if (presentation.lifecycle == ToolPresentationLifecycle::Preparing) {
    presentation.title = "prepare URL fetch";
  } else if (presentation.lifecycle == ToolPresentationLifecycle::Running) {
    presentation.title = "fetching URL";
  } else {
    presentation.title = "web fetch";
  }
  if (!url.empty()) {
    presentation.footer_badges.push_back(url);
  }

  if (presentation.lifecycle == ToolPresentationLifecycle::Error) {
    ApplyError(presentation, view, "web fetch failed");
    return presentation;
  }
  if (presentation.lifecycle != ToolPresentationLifecycle::Success) {
    return presentation;
  }

  rapidjson::Document result_doc;
  if (!ParseObject(view.result, result_doc)) {
    return presentation;
  }
  if (result_doc.HasMember("size") && result_doc["size"].IsUint64()) {
    presentation.footer_badges.push_back(
        std::to_string(result_doc["size"].GetUint64()) + " bytes");
    presentation.facts.push_back(
        {"Size", std::to_string(result_doc["size"].GetUint64()) + " bytes"});
  }
  const std::string redirected = StringMember(result_doc, "redirected_to");
  if (!redirected.empty()) {
    presentation.notices.push_back(
        {ToolPresentationNoticeKind::Info, "Large response saved to " + redirected});
  }
  const std::string instruction = StringMember(result_doc, "instruction");
  if (!instruction.empty()) {
    presentation.notices.push_back(
        {ToolPresentationNoticeKind::Info, "Follow-up: " + instruction});
    ToolPresentationSection followup;
    followup.title = "Follow-up";
    followup.lines.push_back(instruction);
    presentation.sections.push_back(std::move(followup));
    presentation.expandable = true;
    presentation.expanded = view.show_result;
  }
  const std::string content = StringMember(result_doc, "content");
  std::istringstream stream(content);
  std::string line;
  std::vector<std::string> lines;
  while (std::getline(stream, line)) {
    lines.push_back(line);
  }
  if (!lines.empty()) {
    ToolPresentationSection section;
    section.title = "Excerpt";
    const size_t shown = std::min<size_t>(8, lines.size());
    section.lines.assign(lines.begin(), lines.begin() + static_cast<long>(shown));
    presentation.body_lines = section.lines;
    presentation.sections.push_back(std::move(section));
    if (lines.size() > shown) {
      presentation.expandable = true;
      presentation.expanded = view.show_result;
      presentation.notices.push_back(
          {ToolPresentationNoticeKind::Info,
           "Showing first " + std::to_string(shown) + " lines"});
    }
  }
  return presentation;
}

} // namespace firmius::tui

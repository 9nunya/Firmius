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
  return IsMatch(tool_name, "web_search");
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

  const rapidjson::Value* results_array = nullptr;
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

} // namespace firmius::tui

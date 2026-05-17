#include "tools/ListPresenter.hpp"
#include "items/ToolCallItem.hpp"
#include "Terminal.hpp"
#include "ThemeAnsi.hpp"

#include <rapidjson/document.h>

namespace firmius::tui2 {

bool ListPresenter::matches(const std::string& toolName) const {
  return toolName == "List";
}

namespace {

std::string parsePath(const std::string& json) {
  if (json.empty()) return "";
  rapidjson::Document doc;
  doc.Parse(json.c_str());
  if (doc.HasParseError() || !doc.IsObject()) return "";
  if (doc.HasMember("path") && doc["path"].IsString()) return doc["path"].GetString();
  return "";
}

int parseEntryCount(const std::string& json) {
  if (json.empty()) return 0;
  rapidjson::Document doc;
  doc.Parse(json.c_str());
  if (doc.HasParseError()) return 0;
  if (doc.IsArray()) return static_cast<int>(doc.Size());
  if (doc.IsObject() && doc.HasMember("count") && doc["count"].IsInt()) return doc["count"].GetInt();
  return 0;
}

} // namespace

std::vector<std::string> ListPresenter::render(const ToolCallItem& item, const ToolRenderContext& /*ctx*/, int /*width*/) const {
  if (item.phase() == ToolPhase::Preparing) {
    return {theme_ansi::warning("  \xe2\x9a\x99 List")};
  }

  std::string path = parsePath(item.args());

  if (item.phase() == ToolPhase::Called) {
    return {theme_ansi::warning("  \xe2\x9a\x99 Listing " + path + "...")};
  }

  // Finished
  int entryCount = parseEntryCount(item.result());
  bool success = item.success();
  std::string icon = success ? "\xe2\x9c\x93" : "\xe2\x9c\x97";

  std::string text = "  " + icon + " Listed " + path + " \xe2\x80\x94 " +
                     std::to_string(entryCount) + " entries";
  return {success ? theme_ansi::success(text) : theme_ansi::error(text)};
}

} // namespace firmius::tui2

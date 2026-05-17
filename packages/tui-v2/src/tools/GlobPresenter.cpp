#include "tools/GlobPresenter.hpp"
#include "items/ToolCallItem.hpp"
#include "Terminal.hpp"
#include "ThemeAnsi.hpp"

#include <rapidjson/document.h>

namespace firmius::tui2 {

bool GlobPresenter::matches(const std::string& toolName) const {
  return toolName == "Glob";
}

namespace {

struct GlobArgs {
  std::string path;
  std::string glob;
};

GlobArgs parseArgs(const std::string& json) {
  GlobArgs a;
  if (json.empty()) return a;
  rapidjson::Document doc;
  doc.Parse(json.c_str());
  if (doc.HasParseError() || !doc.IsObject()) return a;
  if (doc.HasMember("path") && doc["path"].IsString()) a.path = doc["path"].GetString();
  if (doc.HasMember("glob") && doc["glob"].IsString()) a.glob = doc["glob"].GetString();
  else if (doc.HasMember("pattern") && doc["pattern"].IsString()) a.glob = doc["pattern"].GetString();
  return a;
}

struct GlobResult {
  int matchCount = 0;
  bool budgetHit = false;
};

GlobResult parseResult(const std::string& json) {
  GlobResult r;
  if (json.empty()) return r;
  rapidjson::Document doc;
  doc.Parse(json.c_str());
  if (doc.HasParseError() || !doc.IsObject()) return r;
  if (doc.HasMember("results") && doc["results"].IsArray()) r.matchCount = static_cast<int>(doc["results"].Size());
  if (r.matchCount == 0 && doc.HasMember("count") && doc["count"].IsInt()) r.matchCount = doc["count"].GetInt();
  if (doc.HasMember("budget_hit") && doc["budget_hit"].IsBool()) r.budgetHit = doc["budget_hit"].GetBool();
  return r;
}

} // namespace

std::vector<std::string> GlobPresenter::render(const ToolCallItem& item, const ToolRenderContext& /*ctx*/, int /*width*/) const {
  if (item.phase() == ToolPhase::Preparing) {
    return {theme_ansi::warning("  \xe2\x9a\x99 Glob")};
  }

  auto args = parseArgs(item.args());

  if (item.phase() == ToolPhase::Called) {
    return {theme_ansi::warning("  \xe2\x9a\x99 Glob \"" + args.glob + "\" in " + args.path + "...")};
  }

  // Finished
  auto res = parseResult(item.result());
  bool success = item.success();
  std::string icon = success ? "\xe2\x9c\x93" : "\xe2\x9c\x97";

  std::string text = "  " + icon + " Glob";
  if (!args.glob.empty()) text += " \"" + args.glob + "\"";
  if (!args.path.empty()) text += " in " + args.path;
  text += " \xe2\x80\x94 " + std::to_string(res.matchCount) + " files";
  if (res.budgetHit) text += " \xe2\x80\x94 budget hit";
  return {success ? theme_ansi::success(text) : theme_ansi::error(text)};
}

} // namespace firmius::tui2

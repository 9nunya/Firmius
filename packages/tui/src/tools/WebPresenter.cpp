#include "tools/WebPresenter.hpp"
#include "tools/ToolArgsParser.hpp"
#include "items/ToolCallItem.hpp"
#include "Terminal.hpp"
#include "ThemeAnsi.hpp"

#include <rapidjson/document.h>
#include <chrono>
#include <iomanip>
#include <sstream>

namespace firmius::tui {

bool WebPresenter::matches(const std::string& toolName) const {
  return toolName == "Web";
}

namespace {

std::string formatDuration(std::chrono::milliseconds ms) {
  double secs = static_cast<double>(ms.count()) / 1000.0;
  std::ostringstream oss;
  oss << std::fixed << std::setprecision(1) << secs << "s";
  return oss.str();
}

} // namespace

std::vector<std::string> WebPresenter::render(const ToolCallItem& item, const ToolRenderContext& /*ctx*/, int /*width*/) const {
  if (item.phase() == ToolPhase::Preparing) {
    return {theme_ansi::warning("  \xe2\x9a\x99 Web")};
  }

  std::string action;
  std::string query;
  std::string url;
  if (!item.args().empty()) {
    rapidjson::Document doc;
    doc.Parse(item.args().c_str());
    if (!doc.HasParseError() && doc.IsObject()) {
      if (doc.HasMember("action") && doc["action"].IsString()) action = doc["action"].GetString();
      if (doc.HasMember("query") && doc["query"].IsString()) query = doc["query"].GetString();
      if (doc.HasMember("url") && doc["url"].IsString()) url = doc["url"].GetString();
    }
  }

  if (item.phase() == ToolPhase::Called) {
    if (action == "Search" || action.empty()) {
      return {theme_ansi::warning("  \xe2\x9a\x99 Searching: \"" + query + "\"..."),
              theme_ansi::dim("  " + formatDuration(item.elapsed()))};
    }
    if (action == "Fetch") {
      return {theme_ansi::warning("  \xe2\x9a\x99 Fetching " + url + "..."),
              theme_ansi::dim("  " + formatDuration(item.elapsed()))};
    }
    return {theme_ansi::warning("  \xe2\x9a\x99 Web." + action)};
  }

  // Finished
  bool success = item.success();
  auto color = [&](const std::string& s) {
    return success ? theme_ansi::success(s) : theme_ansi::error(s);
  };
  std::string icon = success ? "\xe2\x9c\x93" : "\xe2\x9c\x97";

  if (!item.result().empty()) {
    rapidjson::Document doc;
    doc.Parse(item.result().c_str());
    if (!doc.HasParseError() && doc.IsObject()) {
      if (action == "Search" || action.empty()) {
        int resultCount = 0;
        std::string provider;
        if (doc.HasMember("results") && doc["results"].IsArray()) resultCount = doc["results"].Size();
        if (doc.HasMember("count") && doc["count"].IsInt()) resultCount = doc["count"].GetInt();
        if (doc.HasMember("provider") && doc["provider"].IsString()) provider = doc["provider"].GetString();
        std::string text = "  " + icon + " Search";
        if (!query.empty()) text += " \"" + query + "\"";
        text += " \xe2\x80\x94 " + std::to_string(resultCount) + " sources";
        if (!provider.empty()) text += " via " + provider;
        return {color(text)};
      }
      if (action == "Fetch") {
        std::string text = "  " + icon + " Fetched";
        if (!url.empty()) text += " " + url;
        // Check for size or redirect
        double size = 0;
        std::string redirected;
        if (doc.HasMember("size") && doc["size"].IsNumber()) size = doc["size"].GetDouble();
        if (doc.HasMember("redirected_to") && doc["redirected_to"].IsString()) redirected = doc["redirected_to"].GetString();
        if (!redirected.empty()) {
          text += " \xe2\x80\x94 too large, saved to " + redirected;
        } else if (size > 0) {
          std::ostringstream oss;
          oss << std::fixed << std::setprecision(1) << (size / 1024.0) << " KB";
          text += " \xe2\x80\x94 " + oss.str();
        }
        return {color(text)};
      }
    }
  }

  return {color("  " + icon + " Web." + action)};
}

} // namespace firmius::tui

#include "tools/TodoPresenter.hpp"
#include "items/ToolCallItem.hpp"
#include "Terminal.hpp"
#include "ThemeAnsi.hpp"

#include <rapidjson/document.h>
#include <sstream>

namespace firmius::tui2 {

bool TodoPresenter::matches(const std::string& toolName) const {
  return toolName == "Todo";
}

std::vector<std::string> TodoPresenter::render(const ToolCallItem& item, const ToolRenderContext& /*ctx*/, int /*width*/) const {
  if (item.phase() == ToolPhase::Preparing) {
    return {theme_ansi::warning("  \xe2\x9a\x99 Todo")};
  }

  if (item.phase() == ToolPhase::Called) {
    return {theme_ansi::warning("  \xe2\x9a\x99 Todo update...")};
  }

  // Finished
  bool success = item.success();
  std::string icon = success ? "\xe2\x9c\x93" : "\xe2\x9c\x97";

  std::vector<std::string> result;
  result.push_back(success ? theme_ansi::success("  " + icon + " Todo updated")
                           : theme_ansi::error("  " + icon + " Todo update failed"));

  // Parse summary from result and show as body
  if (!item.result().empty()) {
    rapidjson::Document doc;
    doc.Parse(item.result().c_str());
    if (!doc.HasParseError() && doc.IsObject()) {
      // Try to get items array
      if (doc.HasMember("items") && doc["items"].IsArray()) {
        for (const auto& todoItem : doc["items"].GetArray()) {
          if (!todoItem.IsObject()) continue;
          std::string status;
          std::string text;
          if (todoItem.HasMember("status") && todoItem["status"].IsString()) status = todoItem["status"].GetString();
          if (todoItem.HasMember("text") && todoItem["text"].IsString()) text = todoItem["text"].GetString();
          if (text.empty()) continue;

          std::string marker;
          std::string styled;
          if (status == "done") {
            marker = "[x]";
            styled = theme_ansi::success(marker);
          } else if (status == "in_progress") {
            marker = "[*]";
            styled = theme_ansi::warning(marker);
          } else {
            marker = "[ ]";
            styled = ansi::dim(marker);
          }
          result.push_back("  " + styled + " " + text);
        }
      } else if (doc.HasMember("summary") && doc["summary"].IsString()) {
        // Fallback: show summary text
        std::string summary = doc["summary"].GetString();
        std::istringstream stream(summary);
        std::string line;
        while (std::getline(stream, line)) {
          result.push_back(theme_ansi::dim("  " + line));
        }
      }
    }
  }

  return result;
}

} // namespace firmius::tui2

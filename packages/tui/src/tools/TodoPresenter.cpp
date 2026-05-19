#include "tools/TodoPresenter.hpp"
#include "AppState.hpp"
#include "items/ToolCallItem.hpp"
#include "Terminal.hpp"
#include "ThemeAnsi.hpp"

#include <rapidjson/document.h>
#include <sstream>

namespace firmius::tui {

namespace {

std::string pluralize(const std::string& noun, int count) {
  return std::to_string(count) + " " + noun + (count == 1 ? "" : "s");
}

std::string joinParts(const std::vector<std::string>& parts) {
  std::string out;
  for (size_t i = 0; i < parts.size(); ++i) {
    if (i > 0) out += ", ";
    out += parts[i];
  }
  return out;
}

std::string summarizeTodoItems(const rapidjson::Value& items) {
  int pending = 0;
  int inProgress = 0;
  int done = 0;
  for (const auto& todoItem : items.GetArray()) {
    if (!todoItem.IsObject() || !todoItem.HasMember("status") ||
        !todoItem["status"].IsString()) {
      continue;
    }
    const std::string status = todoItem["status"].GetString();
    if (status == "done") {
      ++done;
    } else if (status == "in_progress") {
      ++inProgress;
    } else {
      ++pending;
    }
  }

  std::vector<std::string> parts;
  if (pending > 0) parts.push_back(pluralize("pending item", pending));
  if (inProgress > 0) parts.push_back(pluralize("active item", inProgress));
  if (done > 0) parts.push_back(pluralize("done item", done));
  if (parts.empty()) return "No todo items";
  return joinParts(parts);
}

} // namespace

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
      if (doc.HasMember("items") && doc["items"].IsArray()) {
        result.push_back(theme_ansi::dim("  " + summarizeTodoItems(doc["items"])));
      } else if (doc.HasMember("summary") && doc["summary"].IsString()) {
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

} // namespace firmius::tui

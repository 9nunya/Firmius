#include "tools/McpPresenter.hpp"
#include "items/ToolCallItem.hpp"
#include "Terminal.hpp"
#include "ThemeAnsi.hpp"

#include <rapidjson/document.h>

namespace firmius::tui2 {

bool McpPresenter::matches(const std::string& toolName) const {
  return toolName.rfind("mcp__", 0) == 0;  // starts with "mcp__"
}

std::vector<std::string> McpPresenter::render(const ToolCallItem& item, const ToolRenderContext& /*ctx*/, int /*width*/) const {
  // Parse server and tool name from mcp__<server>__<tool>
  std::string server;
  std::string tool;
  const std::string& name = item.toolName();
  if (name.rfind("mcp__", 0) == 0) {
    size_t firstSep = 5;  // after "mcp__"
    size_t secondSep = name.find("__", firstSep);
    if (secondSep != std::string::npos) {
      server = name.substr(firstSep, secondSep - firstSep);
      tool = name.substr(secondSep + 2);
    } else {
      server = name.substr(5);
    }
  }

  std::string displayName = server;
  if (!tool.empty()) displayName += ": " + tool;

  if (item.phase() == ToolPhase::Preparing) {
    return {theme_ansi::warning("  \xe2\x9a\x99 " + displayName)};
  }

  if (item.phase() == ToolPhase::Called) {
    std::vector<std::string> result;
    result.push_back(theme_ansi::warning("  \xe2\x9a\x99 " + displayName));

    // Show truncated input overview — top-level keys, 4 chars per value
    if (!item.args().empty()) {
      rapidjson::Document doc;
      doc.Parse(item.args().c_str());
      if (!doc.HasParseError() && doc.IsObject()) {
        std::string overview;
        for (auto it = doc.MemberBegin(); it != doc.MemberEnd(); ++it) {
          if (!overview.empty()) overview += " | ";
          std::string key = it->name.GetString();
          std::string val;
          if (it->value.IsString()) {
            val = it->value.GetString();
          } else if (it->value.IsInt()) {
            val = std::to_string(it->value.GetInt());
          } else if (it->value.IsBool()) {
            val = it->value.GetBool() ? "true" : "false";
          } else {
            val = "...";
          }
          if (val.size() > 4) val = val.substr(0, 4) + "...";
          overview += key + ": " + val;
        }
        if (!overview.empty()) {
          result.push_back(theme_ansi::dim("  " + overview));
        }
      }
    }
    return result;
  }

  // Finished
  if (item.success()) {
    return {theme_ansi::success("  \xe2\x9c\x93 " + displayName + " \xe2\x80\x94 done")};
  }
  return {theme_ansi::error("  \xe2\x9c\x97 " + displayName + " \xe2\x80\x94 failed")};
}

} // namespace firmius::tui2

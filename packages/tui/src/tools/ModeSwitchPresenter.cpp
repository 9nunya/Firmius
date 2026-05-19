#include "tools/ModeSwitchPresenter.hpp"
#include "items/ToolCallItem.hpp"
#include "Terminal.hpp"
#include "ThemeAnsi.hpp"

#include <rapidjson/document.h>

namespace firmius::tui {

bool ModeSwitchPresenter::matches(const std::string& toolName) const {
  return toolName == "ModeSwitch";
}

std::vector<std::string> ModeSwitchPresenter::render(const ToolCallItem& item, const ToolRenderContext& /*ctx*/, int /*width*/) const {
  if (item.phase() == ToolPhase::Preparing) {
    return {theme_ansi::warning("  \xe2\x9a\x99 ModeSwitch")};
  }

  if (item.phase() == ToolPhase::Called) {
    // Parse name from args
    std::string name;
    if (!item.args().empty()) {
      rapidjson::Document doc;
      doc.Parse(item.args().c_str());
      if (!doc.HasParseError() && doc.IsObject() && doc.HasMember("name") && doc["name"].IsString()) {
        name = doc["name"].GetString();
      }
    }
    if (name.empty()) {
      return {theme_ansi::warning("  \xe2\x86\x92 Clearing mode...")};
    }
    return {theme_ansi::warning("  \xe2\x86\x92 Switching to " + name + "...")};
  }

  // Finished
  std::string toMode;
  std::string reason;
  if (!item.result().empty()) {
    rapidjson::Document doc;
    doc.Parse(item.result().c_str());
    if (!doc.HasParseError() && doc.IsObject()) {
      if (doc.HasMember("to_mode") && doc["to_mode"].IsString()) toMode = doc["to_mode"].GetString();
    }
  }
  if (!item.args().empty()) {
    rapidjson::Document doc;
    doc.Parse(item.args().c_str());
    if (!doc.HasParseError() && doc.IsObject()) {
      if (doc.HasMember("reason") && doc["reason"].IsString()) reason = doc["reason"].GetString();
    }
  }

  std::vector<std::string> result;
  if (toMode.empty()) {
    result.push_back(theme_ansi::success("  \xe2\x86\x92 Cleared mode"));
  } else {
    result.push_back(theme_ansi::success("  \xe2\x86\x92 " + toMode));
  }
  if (!reason.empty()) {
    result.push_back(theme_ansi::dim("  " + reason));
  }
  return result;
}

} // namespace firmius::tui

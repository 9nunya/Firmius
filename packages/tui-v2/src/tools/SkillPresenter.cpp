#include "tools/SkillPresenter.hpp"
#include "items/ToolCallItem.hpp"
#include "Terminal.hpp"
#include "ThemeAnsi.hpp"

#include <rapidjson/document.h>

namespace firmius::tui2 {

bool SkillPresenter::matches(const std::string& toolName) const {
  return toolName == "Skill";
}

std::vector<std::string> SkillPresenter::render(const ToolCallItem& item, const ToolRenderContext& /*ctx*/, int /*width*/) const {
  if (item.phase() == ToolPhase::Preparing) {
    return {theme_ansi::warning("  \xe2\x9a\x99 Skill")};
  }

  if (item.phase() == ToolPhase::Called) {
    std::string what;
    if (!item.args().empty()) {
      rapidjson::Document doc;
      doc.Parse(item.args().c_str());
      if (!doc.HasParseError() && doc.IsObject() && doc.HasMember("what") && doc["what"].IsString()) {
        what = doc["what"].GetString();
      }
    }
    return {theme_ansi::warning("  \xe2\x9a\x99 Loading skill " + what + "...")};
  }

  // Finished
  std::string name;
  if (!item.result().empty()) {
    rapidjson::Document doc;
    doc.Parse(item.result().c_str());
    if (!doc.HasParseError() && doc.IsObject() && doc.HasMember("name") && doc["name"].IsString()) {
      name = doc["name"].GetString();
    }
  }
  if (name.empty()) name = "skill";

  if (item.success()) {
    return {theme_ansi::success("  \xe2\x9c\x93 Skill " + name + " loaded")};
  }
  return {theme_ansi::error("  \xe2\x9c\x97 Skill load failed")};
}

} // namespace firmius::tui2

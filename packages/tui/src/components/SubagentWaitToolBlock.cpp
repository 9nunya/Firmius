#include "components/SubagentWaitToolBlock.hpp"
#include "components/GlintEffect.hpp"
#include "utils/ToolSummaries.hpp"
#include <ftxui/dom/elements.hpp>
#include <rapidjson/document.h>

namespace firmius::tui {

ftxui::Component
SubagentWaitToolBlock(const std::shared_ptr<ToolCallView> &view) {
  return ftxui::Renderer([view] {
    if (!view)
      return ftxui::text("Waiting on subagent...") | ftxui::dim;

    // Humanize the summary
    std::string title = "subagent";
    if (!view->subagent_title.empty()) {
      title = view->subagent_title;
    } else if (!view->args.empty()) {
      rapidjson::Document doc;
      doc.Parse(view->args.c_str());
      if (!doc.HasParseError() && doc.IsObject()) {
        if (doc.HasMember("title") && doc["title"].IsString()) {
          title = doc["title"].GetString();
        } else if (doc.HasMember("name") && doc["name"].IsString()) {
          title = doc["name"].GetString();
        }
      }
    }

    if (view->phase == ToolPhase::Preparing ||
        view->phase == ToolPhase::Called) {
      GlintConfig cfg;
      cfg.target = GlintConfig::Target::Text;
      cfg.gradientColors = {
          ftxui::Color::RGB(100, 180, 255), ftxui::Color::RGB(200, 230, 255),
          ftxui::Color::White, ftxui::Color::RGB(200, 230, 255),
          ftxui::Color::RGB(100, 180, 255)};
      cfg.glintSize = 14;
      cfg.intervalSeconds = 1.0f;
      cfg.durationSeconds = 2.0f;
      cfg.easing = GlintEasing::EaseInOut;

      auto text = ftxui::text("▸ Waiting on " + title + "...") | ftxui::bold |
                  ftxui::color(ftxui::Color::RGB(150, 200, 255));
      return GlintEffect(text, cfg)->Render();
    }

    // Finished state
    return ftxui::hbox(
        {ftxui::text("▸ ") | ftxui::color(ftxui::Color::RGB(100, 220, 150)),
         ftxui::text(title + " ready") |
             ftxui::color(ftxui::Color::RGB(150, 255, 200)) | ftxui::dim});
  });
}

} // namespace firmius::tui

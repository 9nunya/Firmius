#include "components/SubagentWaitToolBlock.hpp"
#include "AgentRegistry.hpp"
#include "ThemeManager.hpp"
#include "components/GlintEffect.hpp"
#include "utils/Icons.hpp"
#include <ftxui/dom/elements.hpp>
#include <rapidjson/document.h>

namespace firmius::tui {

ftxui::Component
SubagentWaitToolBlock(const std::shared_ptr<ToolCallView> &view) {
  return ftxui::Renderer([view] {
    const auto &theme = ThemeManager::instance().getCurrentTheme();

    if (!view)
      return ftxui::text("Waiting on subagent...") |
             ftxui::color(theme.base.dim);

    // Humanize the summary
    std::string title = "subagent";
    if (!view->subagent_title.empty()) {
      title = view->subagent_title;
    } else if (!view->subagent_id.empty()) {
      auto agent =
          firmius::core::AgentRegistry::instance().getAgent(view->subagent_id);
      if (agent) {
        const auto &ident = agent->getContext().identity;
        title = ident.friendlyName.empty() ? ident.name : ident.friendlyName;
      }
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
      cfg.gradientColors = theme.tool_blocks.glint;
      cfg.glintSize = 14;
      cfg.intervalSeconds = 1.0f;
      cfg.durationSeconds = 2.0f;
      cfg.easing = GlintEasing::EaseInOut;

      using namespace firmius::shared;
      auto text =
          ftxui::text(" " + ICON_WAIT + " Waiting on " + title + "...") |
          ftxui::bold | ftxui::color(theme.tool_blocks.specific.wait.fg);
      return GlintEffect(text, cfg)->Render();
    }

    using namespace firmius::shared;
    // Finished state
    return ftxui::hbox({ftxui::text(" " + ICON_CHECK + " ") |
                            ftxui::color(theme.tool_blocks.specific.wait.fg),
                        ftxui::text(title + " ready") |
                            ftxui::color(theme.tool_blocks.specific.wait.fg) |
                            ftxui::dim});
  });
}

} // namespace firmius::tui

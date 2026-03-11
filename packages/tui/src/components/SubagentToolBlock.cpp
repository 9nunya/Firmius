#include "components/SubagentToolBlock.hpp"
#include "components/LogWindow.hpp"
#include "components/GlintEffect.hpp"
#include <ftxui/dom/elements.hpp>
#include <rapidjson/document.h>

namespace firmius::tui {

ftxui::Component SubagentToolBlock(const std::shared_ptr<ToolCallView> &view) {
  auto opt = ftxui::ButtonOption::Simple();
  opt.transform = [](const ftxui::EntryState &s) {
    auto e = ftxui::text(s.label) | ftxui::dim;
    if (s.focused)
      e = e | ftxui::underlined;
    return e;
  };
  if (view) {
    opt.label = &view->toggle_label;
  } else {
    opt.label = "expand";
  }
  opt.on_click = [view] {
    if (!view)
      return;
    view->show_result = !view->show_result;
  };

  auto toggle = ftxui::Button(opt);
  auto container = ftxui::Container::Horizontal({toggle});

  return ftxui::Renderer(container, [view, toggle] {
    if (!view)
      return ftxui::text("Subagent call") | ftxui::dim;

    // Extract args
    std::string title = view->subagent_title;
    std::string task;
    std::string persona;
    bool is_async = false;
    if (!view->args.empty()) {
      rapidjson::Document doc;
      doc.Parse(view->args.c_str());
      if (!doc.HasParseError() && doc.IsObject()) {
        if (title.empty() && doc.HasMember("title") && doc["title"].IsString())
          title = doc["title"].GetString();
        if (doc.HasMember("task") && doc["task"].IsString())
          task = doc["task"].GetString();
        if (doc.HasMember("persona") && doc["persona"].IsString())
          persona = doc["persona"].GetString();
        if (title.empty() && doc.HasMember("name") && doc["name"].IsString())
          title = doc["name"].GetString();
        if (doc.HasMember("async") && doc["async"].IsBool())
          is_async = doc["async"].GetBool();
      }
    }

    if (title.empty())
      title = "subagent";

    // Build footer label
    std::string footer_label = title;
    if (!persona.empty()) footer_label += " [" + persona;
    if (is_async) footer_label += (persona.empty() ? "async]" : ", async]");
    else if (!persona.empty()) footer_label += "]";

    // ── Preparing state with glint effect ──
    if (view->phase == ToolPhase::Preparing) {
      GlintConfig cfg;
      cfg.target = GlintConfig::Target::Text;
      cfg.gradientColors = {ftxui::Color::RGB(180, 120, 255), ftxui::Color::White};
      cfg.glintSize = 12;
      cfg.intervalSeconds = 2;
      cfg.durationSeconds = 1.5f;
      cfg.easing = GlintEasing::EaseInOut;
      
      auto spawning_text = ftxui::text("⟳ Spawning \"" + title + "\"...") | ftxui::bold;
      auto spawning = GlintEffect(spawning_text, cfg);
      
      return ftxui::hbox({
        ftxui::text("▸ ") | ftxui::color(ftxui::Color::RGB(180, 120, 255)),
        spawning->Render()
      });
    }

    bool status_spawned = false;
    if (view->phase == ToolPhase::Finished && view->success && !view->result.empty()) {
      rapidjson::Document res;
      res.Parse(view->result.c_str());
      if (!res.HasParseError() && res.IsObject() && res.HasMember("status") && res["status"].IsString()) {
        std::string status = res["status"].GetString();
        if (status == "spawned" || status == "re-tasked") {
          status_spawned = true;
        }
      }
    }

    // ── Running state: show title + task + rolling tool log (expandable) ──
    if (view->phase == ToolPhase::Called || view->subagent_running || status_spawned) {
      std::vector<ftxui::Element> rows;

      // Header with glint effect
      GlintConfig cfg;
      cfg.target = GlintConfig::Target::Text;
      cfg.gradientColors = {ftxui::Color::RGB(180, 120, 255), ftxui::Color::RGB(220, 180, 255)};
      cfg.glintSize = 10;
      cfg.intervalSeconds = 3;
      cfg.durationSeconds = 2.0f;
      cfg.easing = GlintEasing::EaseInOut;
      
      auto header_text = ftxui::text("▸ " + title) | ftxui::bold | ftxui::color(ftxui::Color::RGB(200, 150, 255));
      auto header_glint = GlintEffect(header_text, cfg);
      
      rows.push_back(header_glint->Render());
      
      // Task description
      if (!task.empty()) {
        std::string task_display = task;
        if (task_display.size() > 80) {
          task_display = task_display.substr(0, 77) + "…";
        }
        rows.push_back(ftxui::text("  " + task_display) | ftxui::dim | ftxui::color(ftxui::Color::RGB(180, 160, 200)));
      }
      
      rows.push_back(ftxui::separatorLight());

      // Tool log - expandable up to 10 lines
      std::vector<ftxui::Element> log_lines;
      size_t log_limit = view->show_result ? 10 : 3;
      
      if (!view->subagent_tool_log.empty()) {
        size_t start_idx = view->show_result ? 0 : 
                          (view->subagent_tool_log.size() > log_limit ? 
                           view->subagent_tool_log.size() - log_limit : 0);
        
        for (size_t i = start_idx; i < view->subagent_tool_log.size(); ++i) {
          log_lines.push_back(ftxui::text("  " + view->subagent_tool_log[i]) | 
                             ftxui::color(ftxui::Color::RGB(160, 140, 180)));
        }
      } else {
        log_lines.push_back(ftxui::text("  running…") | ftxui::dim | ftxui::color(ftxui::Color::RGB(150, 150, 180)));
      }
      
      // Toggle button for expand/collapse
      view->toggle_label = view->show_result ? "collapse" : "expand";
      auto toggle_row = ftxui::hbox({
        ftxui::text("  [") | ftxui::dim,
        toggle->Render(),
        ftxui::text("]") | ftxui::dim
      });
      log_lines.push_back(toggle_row);
      
      rows.push_back(ftxui::vbox(log_lines) | ftxui::frame);
      
      return ftxui::vbox(rows) | ftxui::borderRounded | ftxui::color(ftxui::Color::RGB(180, 150, 200));
    }

    // ── Finished state ──
    std::vector<ftxui::Element> rows;

    if (view->success) {
      view->toggle_label = view->show_result ? "hide" : "show";

      // Success header
      rows.push_back(ftxui::hbox({
        ftxui::text("▸ ") | ftxui::color(ftxui::Color::RGB(100, 220, 150)),
        ftxui::text(title + " completed") | ftxui::bold | ftxui::color(ftxui::Color::RGB(150, 255, 200)),
        ftxui::text("  [") | ftxui::dim,
        toggle->Render(),
        ftxui::text("]") | ftxui::dim
      }));

      if (view->show_result && !view->result.empty()) {
        std::string display_result = view->result;
        rapidjson::Document res;
        res.Parse(view->result.c_str());
        if (!res.HasParseError() && res.IsObject() && res.HasMember("result") &&
            res["result"].IsString()) {
          display_result = res["result"].GetString();
        }

        if (display_result.size() > 300) {
          display_result = display_result.substr(0, 297) + "…";
        }

        std::vector<ftxui::Element> result_lines;
        for (const auto& line : display_result) {
          (void)line; // suppress warning
        }
        result_lines.push_back(ftxui::text(display_result) | 
                              ftxui::color(ftxui::Color::RGB(180, 200, 190)));

        rows.push_back(ftxui::separatorLight());
        rows.push_back(ftxui::vbox(result_lines) | ftxui::frame | ftxui::size(ftxui::HEIGHT, ftxui::LESS_THAN, 10));
      }

      // Tool log from subagent
      if (!view->subagent_tool_log.empty()) {
        rows.push_back(ftxui::separatorLight());
        std::vector<ftxui::Element> log_lines;
        size_t start = view->show_result ? 0 : 
                      (view->subagent_tool_log.size() > 5 ? 
                       view->subagent_tool_log.size() - 5 : 0);
        for (size_t i = start; i < view->subagent_tool_log.size(); ++i) {
          log_lines.push_back(ftxui::text("  " + view->subagent_tool_log[i]) | 
                             ftxui::color(ftxui::Color::RGB(160, 140, 180)));
        }
        rows.push_back(ftxui::vbox(log_lines) | ftxui::frame);
      }
      
      return ftxui::vbox(rows) | ftxui::borderRounded | ftxui::color(ftxui::Color::RGB(150, 200, 180));
    } else {
      // Error state
      std::string error_msg = view->result;
      if (error_msg.empty())
        error_msg = "unknown error";
      if (error_msg.size() > 80)
        error_msg = error_msg.substr(0, 77) + "…";

      return ftxui::hbox({
        ftxui::text("▸ ") | ftxui::color(ftxui::Color::Red),
        ftxui::text(title + " failed: " + error_msg) | ftxui::color(ftxui::Color::RedLight)
      }) | ftxui::borderRounded | ftxui::color(ftxui::Color::RGB(200, 100, 100));
    }
  });
}

} // namespace firmius::tui

#include "components/SubagentToolBlock.hpp"
#include "StreamStateManager.hpp"
#include "ThemeManager.hpp"
#include "components/GlintEffect.hpp"
#include "utils/ErrorCleaner.hpp"
#include "utils/Icons.hpp"
#include "utils/ToolSummaries.hpp"
#include "utils/ToolView.hpp"
#include <ftxui/dom/elements.hpp>
#include <rapidjson/document.h>

namespace firmius::tui {

ftxui::Component SubagentToolBlock(const std::shared_ptr<ToolCallView> &view,
                                   HistoryGetter sub_history_getter,
                                   StreamGetter sub_stream_getter) {
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

  return ftxui::Renderer(container, [view, toggle, sub_history_getter,
                                     sub_stream_getter] {
    if (!view)
      return ftxui::text("Subagent call") | ftxui::dim;

    // Extract args
    std::string title = view->subagent_title;
    std::string task;
    if (!view->args.empty()) {
      rapidjson::Document doc;
      doc.Parse(view->args.c_str());
      if (!doc.HasParseError() && doc.IsObject()) {
        if (title.empty() && doc.HasMember("title") && doc["title"].IsString())
          title = doc["title"].GetString();
        if (doc.HasMember("task") && doc["task"].IsString())
          task = doc["task"].GetString();
        if (title.empty() && doc.HasMember("name") && doc["name"].IsString())
          title = doc["name"].GetString();
      }
    }
    if (title.empty())
      title = "subagent";

    const auto &theme = ThemeManager::instance().getCurrentTheme();

    // ── Generate Synthesized Log ──
    std::vector<shared::SubagentToolLogEntry> synthesized_log;
    if (sub_history_getter && !view->subagent_id.empty()) {
      const auto *hist = sub_history_getter(view->subagent_id);
      if (hist) {
        for (const auto &turn : hist->turns) {
          for (const auto &msg : turn.messages) {
            for (const auto &part : msg.content) {
              if (auto *tc = std::get_if<shared::ToolCallContent>(&part)) {
                shared::SubagentToolLogEntry entry;
                entry.summary = shared::SummarizeToolCall(
                    tc->name, tc->args, shared::ToolPhase::Finished);
                entry.phase = shared::ToolPhase::Finished;
                entry.toolCallId = tc->id;
                synthesized_log.push_back(std::move(entry));
              } else if (auto *th =
                             std::get_if<shared::ThinkingContent>(&part)) {
                if (!th->thinking.empty()) {
                  shared::SubagentToolLogEntry entry;
                  entry.summary = "Thought";
                  entry.phase = shared::ToolPhase::Finished;
                  synthesized_log.push_back(std::move(entry));
                }
              }
            }
          }
        }
      }
    }

    if (sub_stream_getter && !view->subagent_id.empty()) {
      const auto *s = sub_stream_getter(view->subagent_id);
      if (s) {
        if (s->is_thinking) {
          shared::SubagentToolLogEntry entry;
          entry.summary = "Thinking...";
          entry.phase = shared::ToolPhase::Preparing;
          synthesized_log.push_back(std::move(entry));
        } else if (!s->text.empty()) {
          shared::SubagentToolLogEntry entry;
          std::string preview = s->text;
          if (preview.size() > 40)
            preview = preview.substr(0, 37) + "...";
          entry.summary = "Responding: " + preview;
          entry.phase = shared::ToolPhase::Preparing;
          synthesized_log.push_back(std::move(entry));
        }
      }
    }

    if (synthesized_log.empty() && !view->subagent_tool_log.empty()) {
      synthesized_log = view->subagent_tool_log;
    }

    auto renderLogSection = [&](size_t limit) {
      if (synthesized_log.empty()) {
        if (view->phase == ToolPhase::Finished)
          return ftxui::text("  (no activity log)") | ftxui::dim;
        return ftxui::text("  running…") | ftxui::dim |
               ftxui::color(ftxui::Color::RGB(150, 150, 180));
      }

      std::vector<ftxui::Element> log_lines;
      size_t start_idx =
          view->show_result
              ? 0
              : (synthesized_log.size() > limit ? synthesized_log.size() - limit
                                                : 0);

      for (size_t i = start_idx; i < synthesized_log.size(); ++i) {
        const auto &entry = synthesized_log[i];
        if (entry.phase == shared::ToolPhase::Preparing) {
          GlintConfig cfg_log;
          cfg_log.target = GlintConfig::Target::Text;
          cfg_log.gradientColors =
              theme.tool_blocks.glint.empty()
                  ? std::vector<ftxui::Color>{ftxui::Color::RGB(150, 50, 255),
                                              ftxui::Color::RGB(220, 100, 255),
                                              ftxui::Color::White,
                                              ftxui::Color::RGB(220, 100, 255),
                                              ftxui::Color::RGB(150, 50, 255)}
                  : theme.tool_blocks.glint;
          cfg_log.glintSize = 10;
          cfg_log.intervalSeconds = 1.0f;
          cfg_log.durationSeconds = 1.2f;
          cfg_log.easing = GlintEasing::EaseInOut;

          auto preparing_text = ftxui::text("  " + entry.summary) | ftxui::bold;
          log_lines.push_back(GlintEffect(preparing_text, cfg_log)->Render() |
                              ftxui::flex_shrink);
        } else {
          log_lines.push_back(ftxui::text("  " + entry.summary) |
                              ftxui::color(theme.base.dim) |
                              ftxui::flex_shrink);
        }
      }
      return ftxui::vbox(log_lines);
    };

    // ── Preparing ──
    if (view->phase == ToolPhase::Preparing) {
      GlintConfig cfg;
      cfg.target = GlintConfig::Target::Text;
      cfg.gradientColors = theme.tool_blocks.glint;
      cfg.glintSize = 14;
      cfg.intervalSeconds = 1.5f;
      cfg.durationSeconds = 1.2f;
      cfg.easing = GlintEasing::EaseInOut;
      using namespace firmius::shared;
      auto spawning_text =
          ftxui::text(ICON_GEAR + " Spawning \"" + title + "\"...") |
          ftxui::bold;
      return ftxui::hbox(
          {ftxui::text("▸ ") |
               ftxui::color(theme.tool_blocks.specific.subagent.fg),
           GlintEffect(spawning_text, cfg)->Render() | ftxui::flex_shrink});
    }

    bool status_spawned =
        (view->phase == ToolPhase::Finished && view->success &&
         (view->result.find("\"status\":\"spawned\"") != std::string::npos ||
          view->result.find("\"status\":\"re-tasked\"") != std::string::npos));

    // ── Running / Spawned ──
    if (view->phase == ToolPhase::Called || view->subagent_running ||
        status_spawned) {
      std::vector<ftxui::Element> rows;
      GlintConfig cfg;
      cfg.target = GlintConfig::Target::Text;
      cfg.gradientColors = theme.tool_blocks.glint;
      cfg.glintSize = 14;
      cfg.intervalSeconds = 2.0f;
      cfg.durationSeconds = 1.5f;
      cfg.easing = GlintEasing::EaseInOut;

      using namespace firmius::shared;
      rows.push_back(
          GlintEffect(ftxui::text(ICON_AGENT + " " + title) | ftxui::bold |
                          ftxui::color(theme.tool_blocks.specific.subagent.fg),
                      cfg)
              ->Render() |
          ftxui::flex_shrink);

      if (!task.empty()) {
        rows.push_back(ftxui::paragraph("  " + task) |
                       ftxui::color(theme.base.dim) | ftxui::flex_shrink |
                       ftxui::size(ftxui::WIDTH, ftxui::LESS_THAN,
                                   ftxui::Terminal::Size().dimx - 10));
      }
      rows.push_back(ftxui::separatorLight() | ftxui::color(theme.base.border));

      view->toggle_label = view->show_result ? "collapse" : "expand";
      rows.push_back(
          ftxui::vbox(
              {renderLogSection(view->show_result ? 10 : 3),
               ftxui::hbox(
                   {ftxui::text("  [") | ftxui::color(theme.base.dim),
                    toggle->Render(),
                    ftxui::text("]") | ftxui::color(theme.base.dim)})}) |
          ftxui::frame);

      return ftxui::vbox(rows) | ftxui::borderRounded |
             ftxui::color(theme.tool_blocks.generic_border) |
             ftxui::bgcolor(theme.tool_blocks.generic_bg);
    }

    // ── Finished ──
    std::vector<ftxui::Element> rows;
    if (view->success) {
      view->toggle_label = view->show_result ? "hide" : "show";
      using namespace firmius::shared;
      rows.push_back(ftxui::hbox(
          {ftxui::text(" " + ICON_CHECK + " ") |
               ftxui::color(theme.tool_blocks.specific.subagent.fg),
           ftxui::text(title + " completed") | ftxui::bold |
               ftxui::color(theme.tool_blocks.specific.subagent.fg) |
               ftxui::flex_shrink,
           ftxui::text("  [") | ftxui::color(theme.base.dim), toggle->Render(),
           ftxui::text("]") | ftxui::color(theme.base.dim)}));

      if (view->show_result && !view->result.empty()) {
        std::string display_result = view->result;
        rapidjson::Document res;
        res.Parse(view->result.c_str());
        if (!res.HasParseError() && res.IsObject() && res.HasMember("result") &&
            res["result"].IsString()) {
          display_result = res["result"].GetString();
        }
        if (display_result.size() > 300)
          display_result = display_result.substr(0, 297) + "…";
        rows.push_back(ftxui::separatorLight() |
                       ftxui::color(theme.base.border));
        rows.push_back(ftxui::paragraph(display_result) |
                       ftxui::color(theme.base.fg) | ftxui::frame |
                       ftxui::size(ftxui::HEIGHT, ftxui::LESS_THAN, 10));
      }

      rows.push_back(ftxui::separatorLight() | ftxui::color(theme.base.border));
      rows.push_back(renderLogSection(view->show_result ? 10 : 5) |
                     ftxui::frame);

      return ftxui::vbox(rows) | ftxui::borderRounded |
             ftxui::color(theme.tool_blocks.generic_border) |
             ftxui::bgcolor(theme.tool_blocks.generic_bg);
    } else {
      std::string error_msg =
          firmius::shared::ErrorCleaner::clean(view->result);
      if (error_msg.size() > 400)
        error_msg = error_msg.substr(0, 397) + "…";

      return ftxui::vbox(
                 {ftxui::hbox(
                      {ftxui::text(" " + shared::ICON_ERROR + " ") |
                           ftxui::color(theme.status_bar.error.normal.fg),
                       ftxui::text(title + " failed") | ftxui::bold |
                           ftxui::color(theme.status_bar.error.normal.fg)}),
                  ftxui::paragraph("  " + error_msg) |
                      ftxui::color(theme.status_bar.error.normal.fg) |
                      ftxui::flex_shrink}) |
             ftxui::borderRounded |
             ftxui::color(theme.status_bar.error.normal.fg) |
             ftxui::flex_shrink;
    }
  });
}

} // namespace firmius::tui

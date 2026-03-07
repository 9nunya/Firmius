#include "components/SubagentToolBlock.hpp"
#include "components/ToolWindow.hpp"
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
    opt.label = "show";
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
      return ftxui::text("[subagent] <null>") | ftxui::dim;

    // Extract args
    std::string title = view->subagent_title;
    std::string task;
    std::string persona;
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
      }
    }

    if (title.empty())
      title = "subagent";

    // Truncate task to first 60 chars
    std::string task_short = task;
    if (task_short.size() > 60) {
      task_short = task_short.substr(0, 57) + "...";
    }

    // ── Preparing state ──
    if (view->phase == ToolPhase::Preparing) {
      return ftxui::text("[>] Summoning \"" + title + "\"...") | ftxui::dim;
    }

    // ── Called state: show title + task + rolling tool log ──
    if (view->phase == ToolPhase::Called) {
      std::vector<ftxui::Element> rows;

      // Header line
      auto header = ftxui::hbox({ftxui::text("[>] ") | ftxui::bold |
                                     ftxui::color(ftxui::Color::Magenta),
                                 ftxui::text(title) | ftxui::bold,
                                 ftxui::text(" -- ") | ftxui::dim,
                                 ftxui::text(task_short) | ftxui::dim});
      rows.push_back(header);

      // Tool window with rolling 3-line log
      if (!view->subagent_tool_log.empty()) {
        std::vector<ftxui::Element> log_lines;
        for (const auto &entry : view->subagent_tool_log) {
          log_lines.push_back(ftxui::text(entry) | ftxui::dim);
        }
        rows.push_back(ToolWindow(log_lines, "subagent"));
      } else {
        std::vector<ftxui::Element> log_lines;
        log_lines.push_back(ftxui::text("running...") | ftxui::dim);
        rows.push_back(ToolWindow(log_lines, "subagent"));
      }

      return ftxui::vbox(rows);
    }

    // ── Finished state ──
    std::vector<ftxui::Element> rows;

    if (view->success) {
      view->toggle_label = view->show_result ? "hide" : "show result";

      rows.push_back(ftxui::hbox(
          {ftxui::text("[*] Subagent \"" + title + "\" completed") |
               ftxui::bold,
           ftxui::text(" +") | ftxui::dim, ftxui::text(" [") | ftxui::dim,
           toggle->Render(), ftxui::text("]") | ftxui::dim}));

      if (view->show_result && !view->result.empty()) {
        // Try to extract the "result" field from result JSON
        std::string display_result = view->result;
        rapidjson::Document res;
        res.Parse(view->result.c_str());
        if (!res.HasParseError() && res.IsObject() && res.HasMember("result") &&
            res["result"].IsString()) {
          display_result = res["result"].GetString();
        }

        // Truncate very long results
        if (display_result.size() > 200) {
          display_result = display_result.substr(0, 197) + "...";
        }

        std::vector<ftxui::Element> result_lines;
        result_lines.push_back(ftxui::text(display_result) | ftxui::dim);
        rows.push_back(ToolWindow(result_lines, "result"));
      }
    } else {
      // Error state
      std::string error_msg = view->result;
      if (error_msg.empty())
        error_msg = "unknown error";

      rows.push_back(
          ftxui::text("[x] Subagent \"" + title + "\" failed: " + error_msg) |
          ftxui::bold | ftxui::color(ftxui::Color::Red));
    }

    return ftxui::vbox(rows);
  });
}

} // namespace firmius::tui

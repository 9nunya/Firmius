#include "components/SubagentToolBlock.hpp"
#include "components/LogWindow.hpp"
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

    // Truncate task to first 60 chars
    std::string task_short = task;
    if (task_short.size() > 60) {
      task_short = task_short.substr(0, 57) + "...";
    }

    // ── Preparing state ──
    if (view->phase == ToolPhase::Preparing) {
      return ftxui::text("[>] Summoning \"" + title + "\"...") | ftxui::dim;
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

    // ── Called state: show title + task + rolling tool log ──
    if (view->phase == ToolPhase::Called || view->subagent_running || status_spawned) {
      std::vector<ftxui::Element> rows;

      auto header = ftxui::hbox({ftxui::text("[>] ") | ftxui::bold |
                                     ftxui::color(ftxui::Color::Magenta),
                                 ftxui::text(title) | ftxui::bold,
                                 ftxui::text(" -- ") | ftxui::dim,
                                 ftxui::text(task_short) | ftxui::dim});
      rows.push_back(header);

      std::string footer = title;
      if (!persona.empty()) footer += " [" + persona;
      if (is_async) footer += (persona.empty() ? " [async]" : ", async]");
      else if (!persona.empty()) footer += "]";

      if (!view->subagent_tool_log.empty()) {
        std::vector<ftxui::Element> log_lines;
        for (const auto &entry : view->subagent_tool_log) {
          log_lines.push_back(ftxui::text(entry) | ftxui::dim);
        }
        rows.push_back(LogWindow(log_lines, footer));
      } else {
        std::vector<ftxui::Element> log_lines;
        log_lines.push_back(ftxui::text("running...") | ftxui::dim);
        rows.push_back(LogWindow(log_lines, footer));
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

        std::string footer = title;
        if (!persona.empty()) footer += " [" + persona;
        if (is_async) footer += (persona.empty() ? " [async]" : ", async]");
        else if (!persona.empty()) footer += "]";

        rows.push_back(LogWindow(result_lines, footer));
      }

      if (!view->subagent_tool_log.empty()) {
        std::vector<ftxui::Element> log_lines;
        for (const auto &entry : view->subagent_tool_log) {
          log_lines.push_back(ftxui::text(entry) | ftxui::dim);
        }
        std::string log_footer = title + " log";
        if (!persona.empty())
          log_footer += " (" + persona + ")";
        rows.push_back(LogWindow(log_lines, log_footer));
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

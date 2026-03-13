#include "components/ProcessExecuteToolBlock.hpp"
#include "ThemeManager.hpp"
#include "components/ANSIParser.hpp"
#include "components/LogWindow.hpp"
#include "utils/Icons.hpp"
#include "utils/ToolSummaries.hpp"
#include <ftxui/dom/elements.hpp>
#include <rapidjson/document.h>
#include <sstream>

namespace firmius::tui {

using firmius::shared::TailLines;

ftxui::Component
ProcessExecuteToolBlock(const std::shared_ptr<ToolCallView> &view) {
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
      return ftxui::text("Process call") | ftxui::dim;

    // Parse command from args
    std::string command_arg;
    if (!view->args.empty()) {
      rapidjson::Document doc;
      doc.Parse(view->args.c_str());
      if (!doc.HasParseError() && doc.IsObject() && doc.HasMember("command") &&
          doc["command"].IsString()) {
        command_arg = doc["command"].GetString();
      }
    }

    std::string cmd_display = "$ " + command_arg;

    const auto &theme = ThemeManager::instance().getCurrentTheme();

    using namespace firmius::shared;
    // ── Preparing ──
    if (view->phase == ToolPhase::Preparing) {
      return ftxui::hbox(
          {ftxui::text(" " + ICON_GEAR + " ") |
               ftxui::color(theme.tool_blocks.specific.terminal.fg),
           ftxui::text("Running " + cmd_display) |
               ftxui::color(theme.base.dim)});
    }

    // ── Called: show rolling last 5 lines of live output with ANSI colors ──
    if (view->phase == ToolPhase::Called) {
      std::vector<ftxui::Element> rows;

      auto tail = TailLines(view->live_process_output, 5);
      if (tail.empty()) {
        tail.push_back("running…");
      }

      std::vector<ftxui::Element> out_lines;
      for (const auto &line : tail) {
        // Parse ANSI colors from process output
        out_lines.push_back(
            ftxui::hbox({ftxui::text("│ ") | ftxui::color(theme.base.dim),
                         ParseANSI(line)}));
      }

      std::string footer = cmd_display;
      rows.push_back(LogWindow(out_lines, footer));
      return ftxui::vbox(rows) | ftxui::borderRounded |
             ftxui::color(theme.tool_blocks.specific.terminal.fg);
    }

    // ── Finished + error ──
    if (!view->success) {
      std::string err_msg = view->result;
      if (err_msg.empty())
        err_msg = "unknown error";
      if (err_msg.size() > 70)
        err_msg = err_msg.substr(0, 67) + "…";

      using namespace firmius::shared;
      return ftxui::hbox({ftxui::text(" " + ICON_ERROR + " ") |
                              ftxui::color(theme.status_bar.error.normal.fg),
                          ftxui::text(cmd_display + " failed: " + err_msg) |
                              ftxui::color(theme.status_bar.error.normal.fg)}) |
             ftxui::borderRounded |
             ftxui::color(theme.status_bar.error.normal.fg);
    }

    // ── Finished + success: parse result ──
    std::string output_str;
    std::string exit_code_str;
    std::string finish_reason;
    {
      rapidjson::Document res;
      res.Parse(view->result.c_str());
      if (!res.HasParseError() && res.IsObject()) {
        // Process execute returns "stdout"/"stderr" keys
        if (res.HasMember("stdout") && res["stdout"].IsString()) {
          output_str = res["stdout"].GetString();
          if (res.HasMember("stderr") && res["stderr"].IsString()) {
            std::string err = res["stderr"].GetString();
            if (!err.empty())
              output_str += "\n[stderr]\n" + err;
          }
        }
        if (res.HasMember("exit_code") && res["exit_code"].IsInt()) {
          exit_code_str = std::to_string(res["exit_code"].GetInt());
        }
        if (res.HasMember("finish_reason") && res["finish_reason"].IsString()) {
          finish_reason = res["finish_reason"].GetString();
        }
        // Process spawn returns "process_id"
        if (view->name == "process_spawn" && res.HasMember("process_id") &&
            res["process_id"].IsString()) {
          output_str = "Spawned process ID: " +
                       std::string(res["process_id"].GetString());
        }
      } else {
        output_str = view->result;
      }
    }

    view->toggle_label = view->show_result ? "collapse" : "expand";

    // Trim output_str to check for meaningful content
    std::string trimmed_output = output_str;
    trimmed_output.erase(
        trimmed_output.begin(),
        std::find_if(trimmed_output.begin(), trimmed_output.end(),
                     [](unsigned char ch) { return !std::isspace(ch); }));
    trimmed_output.erase(
        std::find_if(trimmed_output.rbegin(), trimmed_output.rend(),
                     [](unsigned char ch) { return !std::isspace(ch); })
            .base(),
        trimmed_output.end());

    if (trimmed_output.empty() && view->success) {
      using namespace firmius::shared;
      return ftxui::hbox(
          {ftxui::text(" " + ICON_CHECK + " ") |
               ftxui::color(theme.tool_blocks.specific.terminal.fg),
           ftxui::text(cmd_display) | ftxui::color(theme.base.dim),
           ftxui::text(" [exit " + exit_code_str + "]") |
               ftxui::color(theme.base.dim)});
    }

    // Count total lines
    int total_lines = 0;
    {
      std::istringstream ss(output_str);
      std::string line;
      while (std::getline(ss, line))
        total_lines++;
    }
    bool has_more = total_lines > 5;

    std::vector<ftxui::Element> out_lines;
    if (view->show_result) {
      // Show all lines with ANSI colors
      std::istringstream ss(output_str);
      std::string line;
      while (std::getline(ss, line)) {
        out_lines.push_back(
            ftxui::hbox({ftxui::text("│ ") | ftxui::color(theme.base.dim),
                         ParseANSI(line)}));
      }
    } else {
      // Show last 5 lines
      auto all_tail = TailLines(output_str, 5);
      for (const auto &line : all_tail) {
        out_lines.push_back(
            ftxui::hbox({ftxui::text("│ ") | ftxui::color(theme.base.dim),
                         ParseANSI(line)}));
      }
    }

    if (out_lines.empty()) {
      out_lines.push_back(ftxui::text("│ (no output)") | ftxui::dim);
    }

    std::string footer = cmd_display;
    if (!exit_code_str.empty()) {
      footer += " [exit " + exit_code_str + "]";
    }
    if (!finish_reason.empty() && finish_reason != "Natural") {
      footer += " (" + finish_reason + ")";
    }

    using namespace firmius::shared;
    std::vector<ftxui::Element> rows;
    rows.push_back(LogWindow(out_lines, ICON_TERMINAL + " " + footer,
                             has_more ? view->toggle_label : ""));
    if (has_more) {
      rows.push_back(ftxui::hbox(
          {ftxui::text("  [") | ftxui::color(theme.base.dim), toggle->Render(),
           ftxui::text("]") | ftxui::color(theme.base.dim)}));
    }

    return ftxui::vbox(rows) | ftxui::borderRounded |
           ftxui::color(theme.tool_blocks.specific.terminal.fg);
  });
}

} // namespace firmius::tui

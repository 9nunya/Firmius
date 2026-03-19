#include "components/ProcessExecuteToolBlock.hpp"
#include "ThemeManager.hpp"
#include "components/ANSIParser.hpp"
#include "components/LogWindow.hpp"
#include "utils/Icons.hpp"
#include "utils/ToolSummaries.hpp"
#include <algorithm>
#include <ftxui/dom/elements.hpp>
#include <cctype>
#include <rapidjson/document.h>
#include <sstream>

namespace firmius::tui {

using firmius::shared::TailLines;
namespace {
const std::string &PythonIcon() { return firmius::shared::ICON_FILE_EDIT; }

struct ProcessResultView {
  std::string output;
  std::string finish_reason;
  std::string exit_code;
};

ProcessResultView parseProcessResult(const std::shared_ptr<ToolCallView> &view) {
  ProcessResultView parsed;
  if (!view) {
    return parsed;
  }

  rapidjson::Document res;
  res.Parse(view->result.c_str());
  if (res.HasParseError() || !res.IsObject()) {
    parsed.output = view->result;
    return parsed;
  }

  if (res.HasMember("stdout") && res["stdout"].IsString()) {
    parsed.output = res["stdout"].GetString();
  }
  if (res.HasMember("stderr") && res["stderr"].IsString()) {
    std::string err = res["stderr"].GetString();
    if (!err.empty()) {
      if (!parsed.output.empty()) {
        parsed.output += "\n";
      }
      parsed.output += "[stderr]\n" + err;
    }
  }
  if (res.HasMember("finish_reason") && res["finish_reason"].IsString()) {
    parsed.finish_reason = res["finish_reason"].GetString();
  }
  if (res.HasMember("exit_code") && res["exit_code"].IsInt()) {
    parsed.exit_code = std::to_string(res["exit_code"].GetInt());
  }
  if (view->name == "process_spawn" && res.HasMember("process_id") &&
      res["process_id"].IsString()) {
    parsed.output = "Spawned process ID: " +
                    std::string(res["process_id"].GetString());
  }

  return parsed;
}
}

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
  auto render_toggle = [toggle](bool visible) -> ftxui::Element {
    if (!visible) {
      return ftxui::text("");
    }
    const auto &theme = ThemeManager::instance().getCurrentTheme();
    return ftxui::hbox({
        ftxui::text(" "),
        toggle->Render() | ftxui::color(theme.base.bg) | ftxui::bold |
            ftxui::bgcolor(theme.status_bar.executing_tool.normal.fg),
        ftxui::text(" "),
    });
  };

  return ftxui::Renderer(container, [view, toggle, render_toggle] {
    if (!view)
      return ftxui::text("Process call") | ftxui::dim;

    std::string command_arg;
    std::string code_arg;
    if (!view->args.empty()) {
      rapidjson::Document doc;
      doc.Parse(view->args.c_str());
      if (!doc.HasParseError() && doc.IsObject()) {
        if (doc.HasMember("command") && doc["command"].IsString()) {
          command_arg = doc["command"].GetString();
        }
        if (doc.HasMember("code") && doc["code"].IsString()) {
          code_arg = doc["code"].GetString();
        }
      }
    }

    const bool is_python = view->name.find("python") != std::string::npos;
    std::string cmd_display = "$ " + command_arg;
    if (is_python) {
      std::istringstream code_stream(code_arg);
      std::string preview;
      while (std::getline(code_stream, preview)) {
        if (!preview.empty()) {
          break;
        }
      }
      if (preview.empty()) {
        preview = "python";
      }
      if (preview.size() > 42) {
        preview = preview.substr(0, 41) + "…";
      }
      cmd_display = "python  " + preview;
    }

    const auto &theme = ThemeManager::instance().getCurrentTheme();
    auto render_output_lines = [&](const std::string &text,
                                   const std::string &placeholder,
                                   bool tail_only) -> std::vector<ftxui::Element> {
      std::vector<ftxui::Element> out_lines;
      std::vector<std::string> raw_lines;
      if (tail_only) {
        raw_lines = TailLines(text, 12);
      } else {
        std::istringstream ss(text);
        std::string line;
        while (std::getline(ss, line)) {
          raw_lines.push_back(line);
        }
      }
      if (raw_lines.empty()) {
        raw_lines.push_back(placeholder);
      }
      for (const auto &line : raw_lines) {
        out_lines.push_back(
            ftxui::hbox({ftxui::text("  ") |
                             ftxui::bgcolor(theme.base.bg),
                         ftxui::text("▏ ") |
                             ftxui::color(theme.tool_blocks.specific.terminal.fg) |
                             ftxui::bgcolor(theme.base.bg),
                         ParseANSI(line) | ftxui::flex}) |
            ftxui::bgcolor(theme.base.bg) | ftxui::xflex);
      }
      return out_lines;
    };

    using namespace firmius::shared;
    if (view->phase == ToolPhase::Preparing) {
      return ftxui::hbox({
                 ftxui::text("▎ ") |
                     ftxui::color(theme.status_bar.executing_tool.normal.fg),
                 ftxui::text(is_python ? "Preparing python" : "Preparing command") |
                     ftxui::color(theme.base.dim),
                 ftxui::text("  " + cmd_display) | ftxui::color(theme.base.fg),
             }) |
             ftxui::bgcolor(theme.tool_blocks.generic_bg) | ftxui::xflex;
    }

    if (view->phase == ToolPhase::Error ||
        (!view->success && view->phase == ToolPhase::Finished)) {
      std::string err_msg = view->result;
      if (err_msg.empty())
        err_msg = "unknown error";
      if (err_msg.size() > 70)
        err_msg = err_msg.substr(0, 67) + "…";

      using namespace firmius::shared;
      return ftxui::vbox({
                 ftxui::hbox({
                     ftxui::text("▎ ") |
                         ftxui::color(theme.status_bar.error.normal.fg),
                     ftxui::text(std::string(is_python ? PythonIcon() : ICON_ERROR) +
                                     " " + cmd_display) |
                         ftxui::bold |
                         ftxui::color(theme.status_bar.error.normal.fg),
                 }) | ftxui::bgcolor(theme.tool_blocks.generic_bg),
                 ftxui::paragraph("  " + err_msg) |
                     ftxui::color(theme.status_bar.error.normal.fg) |
                     ftxui::bgcolor(theme.tool_blocks.generic_bg),
             }) |
             ftxui::bgcolor(theme.tool_blocks.generic_bg) | ftxui::xflex;
    }

    auto parsed = parseProcessResult(view);
    std::string output_str = parsed.output;
    std::string exit_code_str = parsed.exit_code;
    std::string finish_reason = parsed.finish_reason;

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

    const bool is_live = view->phase == ToolPhase::Called;
    const bool is_background = view->phase == ToolPhase::BackgroundRunning;
    const bool finished_in_background =
        view->process_is_background && view->phase == ToolPhase::Finished &&
        view->process_exit_known;
    const bool prefer_live_output =
        is_live || is_background || (finished_in_background && !view->show_result);
    const std::string display_output =
        prefer_live_output ? view->live_process_output : output_str;
    const std::string placeholder =
        is_background ? "running in background, awaiting output…"
        : is_live ? "running, awaiting output…"
                  : "no output";

    if (trimmed_output.empty() && view->success && !prefer_live_output) {
      using namespace firmius::shared;
      return ftxui::hbox({
                 ftxui::text("▎ ") |
                     ftxui::color(theme.status_bar.idle.normal.fg),
                 ftxui::text(std::string(is_python ? PythonIcon() : ICON_CHECK) + " " +
                             cmd_display) |
                     ftxui::color(theme.base.fg),
                 ftxui::text("  [exit " + exit_code_str + "]") |
                     ftxui::color(theme.base.dim),
             }) |
             ftxui::bgcolor(theme.tool_blocks.generic_bg) | ftxui::xflex;
    }

    // Count total lines
    int total_lines = 0;
    {
      std::istringstream ss(display_output);
      std::string line;
      while (std::getline(ss, line))
        total_lines++;
    }
    bool has_more = total_lines > 5;

    auto out_lines =
        render_output_lines(display_output, placeholder, !view->show_result);

    std::string footer = cmd_display;
    if (view->process_exit_known && exit_code_str.empty()) {
      exit_code_str = std::to_string(view->process_exit_code);
    }
    if (!exit_code_str.empty() && !is_live && !is_background) {
      footer += " [exit " + exit_code_str + "]";
    }
    if (!finish_reason.empty() && finish_reason != "Natural" &&
        finish_reason != "Timeout") {
      footer += " (" + finish_reason + ")";
    }

    using namespace firmius::shared;
    std::vector<ftxui::Element> rows;
    auto extra = render_toggle(has_more);
    if (is_live) {
      extra = ftxui::text(" live ") | ftxui::bold |
              ftxui::color(theme.base.bg) |
              ftxui::bgcolor(theme.status_bar.executing_tool.normal.fg);
    } else if (is_background) {
      extra = ftxui::hbox({
          ftxui::text(" moved to background ") | ftxui::bold |
              ftxui::color(theme.base.bg) |
              ftxui::bgcolor(theme.status_bar.executing_tool.normal.fg),
          render_toggle(has_more),
      });
    } else if (finished_in_background) {
      std::string label = " completed in background ";
      if (!exit_code_str.empty()) {
        label += "[exit " + exit_code_str + "] ";
      }
      extra = ftxui::hbox({
          ftxui::text(label) | ftxui::bold |
              ftxui::color(theme.base.bg) |
              ftxui::bgcolor(theme.status_bar.idle.normal.fg),
          render_toggle(has_more),
      });
    }

    rows.push_back(LogWindow(out_lines,
                             std::string(is_python ? PythonIcon() : ICON_TERMINAL) +
                                 " " + footer,
                             extra));

    return ftxui::vbox(rows) | ftxui::bgcolor(theme.tool_blocks.generic_bg) |
           ftxui::xflex;
  });
}

} // namespace firmius::tui

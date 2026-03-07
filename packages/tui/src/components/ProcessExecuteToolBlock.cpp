#include "components/ProcessExecuteToolBlock.hpp"
#include "components/Markdown.hpp"
#include <ftxui/dom/elements.hpp>
#include <rapidjson/document.h>

namespace firmius::tui {

static std::string phaseLabel(ToolPhase phase) {
  switch (phase) {
  case ToolPhase::Preparing:
    return "preparing";
  case ToolPhase::Called:
    return "running";
  case ToolPhase::Finished:
    return "finished";
  }
  return "unknown";
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
      return ftxui::text("[process] <null>") | ftxui::dim;

    std::string tool_type =
        view->name == "process_spawn" ? "process_spawn" : "process_execute";
    std::string head = "[" + tool_type + "] " + phaseLabel(view->phase);

    std::string command_arg = "";
    if (view->args.length() > 0) {
      rapidjson::Document doc;
      doc.Parse(view->args.c_str());
      if (!doc.HasParseError() && doc.IsObject() && doc.HasMember("command") &&
          doc["command"].IsString()) {
        command_arg = doc["command"].GetString();
      }
    }

    if (view->phase == ToolPhase::Preparing ||
        view->phase == ToolPhase::Called) {
      head = head + " `$ " + command_arg + "`";
    } else if (view->phase == ToolPhase::Finished) {
      if (view->success) {
        head = "Ran `$ " + command_arg + "` ✓";
      } else {
        head = head + " `$ " + command_arg + "` ✗";
      }
    }

    bool can_toggle =
        (view->phase == ToolPhase::Finished && !view->result.empty());
    view->toggle_label = view->show_result ? "hide" : "show";

    std::vector<ftxui::Element> rows;
    if (can_toggle || (view->phase == ToolPhase::Called)) {
      rows.push_back(ftxui::hbox(
          {ftxui::text(head) | ftxui::bold, ftxui::text(" [") | ftxui::dim,
           toggle->Render(), ftxui::text("]") | ftxui::dim}));

      if (view->show_result || view->phase == ToolPhase::Called) {
        // Determine output string. Either from live view or from result
        std::string output_str = "";
        if (view->phase == ToolPhase::Finished && !view->result.empty()) {
          rapidjson::Document res;
          res.Parse(view->result.c_str());
          if (!res.HasParseError() && res.IsObject() &&
              res.HasMember("stdout_output") &&
              res["stdout_output"].IsString()) {
            output_str = res["stdout_output"].GetString();
            if (res.HasMember("stderr_output") &&
                res["stderr_output"].IsString()) {
              std::string err_str = res["stderr_output"].GetString();
              if (!err_str.empty()) {
                output_str += "\n[stderr]\n" + err_str;
              }
            }
          } else if (view->name == "process_spawn") {
            if (!res.HasParseError() && res.IsObject() &&
                res.HasMember("process_id") && res["process_id"].IsString()) {
              output_str = "Spawned process ID: " +
                           std::string(res["process_id"].GetString());
            } else {
              output_str = view->result;
            }
          } else {
            output_str = view->result;
          }
        }
        // Note: live streaming of process output requires a connection between
        // the runtime and the TUI. It is very complex without a direct
        // websocket/stream. For now, the "Called" phase will just show
        // 'Running...' or last output known to UI state if streamed.

        if (output_str.empty() && view->phase == ToolPhase::Called) {
          output_str = "Running...";
        }

        if (!output_str.empty()) {
          // Primitive ANSI stripped renderer (could be improved with ftxui text
          // color parsing)
          std::stringstream ss(output_str);
          std::string line;
          std::vector<ftxui::Element> out_lines;
          while (std::getline(ss, line)) {
            // TODO: Strip ANSI or parse it fully.
            out_lines.push_back(ftxui::text(line) | ftxui::dim);
          }
          rows.push_back(ftxui::vbox(std::move(out_lines)) | ftxui::border);
        }
      }
    } else {
      rows.push_back(ftxui::text(head) | ftxui::bold);
      if (view->phase == ToolPhase::Finished && !view->success) {
        rows.push_back(firmius::tui::RenderMarkdown(view->result) |
                       ftxui::color(ftxui::Color::Red));
      }
    }

    return ftxui::vbox(rows);
  });
}

} // namespace firmius::tui

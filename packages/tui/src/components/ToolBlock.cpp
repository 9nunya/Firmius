#include "components/ToolBlock.hpp"
#include "components/FileEditToolBlock.hpp"
#include "components/FileReadToolBlock.hpp"
#include "components/ListDirectoryToolBlock.hpp"
#include "components/ProcessExecuteToolBlock.hpp"
#include "components/SubagentToolBlock.hpp"
#include "components/ToolWindow.hpp"
#include <ftxui/component/component.hpp>
#include <ftxui/dom/elements.hpp>
#include <vector>

namespace firmius::tui {

ftxui::Component ToolBlock(const std::shared_ptr<ToolCallView> &view) {
  if (view) {
    if (view->name == "list_directory") {
      return ListDirectoryToolBlock(view);
    } else if (view->name == "file_read") {
      return FileReadToolBlock(view);
    } else if (view->name == "file_edit") {
      return FileEditToolBlock(view);
    } else if (view->name == "process_execute" ||
               view->name == "process_spawn") {
      return ProcessExecuteToolBlock(view);
    } else if (view->name == "summon_subagent") {
      return SubagentToolBlock(view);
    }
  }

  // ── Generic fallback for unhandled tools ──
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
    if (!view) {
      return ftxui::text("[tool] <null>") | ftxui::dim;
    }

    // Use SummarizeToolCall for a compact description
    std::string summary = SummarizeToolCall(view->name, view->args);

    if (view->phase == ToolPhase::Preparing ||
        view->phase == ToolPhase::Called) {
      return ftxui::text("[~] " + summary + "...") | ftxui::dim;
    }

    // Finished
    if (view->success) {
      view->toggle_label = view->show_result ? "hide" : "show";
      bool can_toggle = !view->result.empty();

      std::vector<ftxui::Element> rows;
      if (can_toggle) {
        rows.push_back(
            ftxui::hbox({ftxui::text("[+] " + summary) | ftxui::bold,
                         ftxui::text(" [") | ftxui::dim, toggle->Render(),
                         ftxui::text("]") | ftxui::dim}));
      } else {
        rows.push_back(ftxui::text("[+] " + summary) | ftxui::bold);
      }

      if (view->show_result && !view->result.empty()) {
        // Show result in a small tool window (last 5 lines)
        auto tail = TailLines(view->result, 5);
        std::vector<ftxui::Element> out_lines;
        for (const auto &line : tail) {
          out_lines.push_back(ftxui::text(line) | ftxui::dim);
        }
        rows.push_back(ToolWindow(out_lines, view->name));
      }

      return ftxui::vbox(rows);
    }

    // Error
    std::string err = view->result;
    if (err.empty())
      err = "unknown error";
    // Truncate error to first 80 chars
    if (err.size() > 80)
      err = err.substr(0, 77) + "...";
    return ftxui::text("[x] " + summary + " failed: " + err) |
           ftxui::color(ftxui::Color::Red);
  });
}

} // namespace firmius::tui

#include "components/ToolBlock.hpp"
#include "components/FileEditToolBlock.hpp"
#include "components/FileReadToolBlock.hpp"
#include "components/ListDirectoryToolBlock.hpp"
#include "components/Markdown.hpp"
#include "components/ProcessExecuteToolBlock.hpp"
#include <ftxui/component/component.hpp>
#include <ftxui/dom/elements.hpp>
#include <vector>

namespace firmius::tui {

static std::string phaseLabel(ToolPhase phase) {
  switch (phase) {
  case ToolPhase::Preparing:
    return "preparing";
  case ToolPhase::Called:
    return "called";
  case ToolPhase::Finished:
    return "finished";
  }
  return "unknown";
}

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
    }
  }

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

    std::string head =
        "[tool] " + view->name + " (" + phaseLabel(view->phase) + ")";
    if (view->phase == ToolPhase::Finished) {
      head += view->success ? " ✓" : " ✗";
    }

    std::vector<ftxui::Element> body_lines;
    if (!view->args.empty()) {
      body_lines.push_back(firmius::tui::RenderMarkdown(view->args));
    }

    bool can_toggle =
        (view->phase == ToolPhase::Finished && !view->result.empty());
    view->toggle_label = view->show_result ? "hide" : "show";
    if (can_toggle && view->show_result) {
      body_lines.push_back(firmius::tui::RenderMarkdown(view->result));
    }

    if (body_lines.empty()) {
      return ftxui::text(head);
    }

    std::vector<ftxui::Element> rows;
    if (can_toggle || (view->phase == ToolPhase::Called)) {
      rows.push_back(ftxui::hbox(
          {ftxui::text(head) | ftxui::bold, ftxui::text(" [") | ftxui::dim,
           toggle->Render(), ftxui::text("]") | ftxui::dim}));
    } else {
      rows.push_back(ftxui::text(head) | ftxui::bold);
    }
    for (auto &line : body_lines)
      rows.push_back(line);

    return ftxui::vbox(rows);
  });
}

} // namespace firmius::tui

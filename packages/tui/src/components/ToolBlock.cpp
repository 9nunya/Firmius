#include "components/ToolBlock.hpp"
#include "components/FileEditToolBlock.hpp"
#include "components/FileReadToolBlock.hpp"
#include "components/ListDirectoryToolBlock.hpp"
#include "components/ProcessExecuteToolBlock.hpp"
#include "components/SubagentToolBlock.hpp"
#include "components/LogWindow.hpp"
#include "components/GlintEffect.hpp"
#include "utils/ToolSummaries.hpp"
#include <ftxui/component/component.hpp>
#include <ftxui/dom/elements.hpp>
#include <rapidjson/document.h>
#include <vector>

namespace firmius::tui {

using firmius::shared::SummarizeToolCall;
using firmius::shared::TailLines;

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
    } else if (view->name == "subagent_wait") {
      return SubagentToolBlock(view);
    }
  }

  // Generic fallback for unhandled tools
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
      return ftxui::text("Tool call") | ftxui::dim;
    }

    std::string summary = SummarizeToolCall(view->name, view->args, view->phase);

    // Preparing state with animated glint
    if (view->phase == ToolPhase::Preparing ||
        view->phase == ToolPhase::Called) {
      
      GlintConfig cfg;
      cfg.target = GlintConfig::Target::Text;
      cfg.gradientColors = {ftxui::Color::RGB(100, 150, 255), ftxui::Color::White};
      cfg.glintSize = 12;
      cfg.intervalSeconds = 2;
      cfg.durationSeconds = 1.5f;
      cfg.easing = GlintEasing::EaseInOut;
      
      auto loading_text = ftxui::text("⟳ " + summary) | ftxui::bold;
      auto loading = GlintEffect(loading_text, cfg);
      
      return ftxui::hbox({
        ftxui::text("▸ ") | ftxui::color(ftxui::Color::RGB(100, 150, 255)),
        loading->Render() | ftxui::color(ftxui::Color::RGB(150, 200, 255))
      }) | ftxui::dim;
    }

    // Success state
    if (view->success) {
      view->toggle_label = view->show_result ? "hide" : "show";
      bool can_toggle = !view->result.empty();

      ftxui::Elements rows;
      
      // Header row with icon and toggle
      ftxui::Elements header;
      header.push_back(ftxui::text("▸ ") | ftxui::color(ftxui::Color::RGB(100, 220, 150)));
      header.push_back(ftxui::text(summary + " ") | ftxui::bold | ftxui::color(ftxui::Color::RGB(150, 255, 200)));
      
      if (can_toggle) {
        header.push_back(ftxui::text("[" ) | ftxui::dim);
        header.push_back(toggle->Render());
        header.push_back(ftxui::text("]") | ftxui::dim);
      }
      
      rows.push_back(ftxui::hbox(header));

      // Expandable result
      if (view->show_result && !view->result.empty()) {
        auto tail = TailLines(view->result, 5);
        ftxui::Elements result_lines;
        for (const auto &line : tail) {
          result_lines.push_back(ftxui::text(line) | ftxui::color(ftxui::Color::GrayLight));
        }
        rows.push_back(ftxui::separatorLight());
        rows.push_back(ftxui::vbox(result_lines) | ftxui::frame | ftxui::size(ftxui::HEIGHT, ftxui::LESS_THAN, 8));
      }

      return ftxui::vbox(rows);
    }

    // Error state
    std::string err = view->result;
    if (err.empty())
      err = "unknown error";
    if (err.size() > 70)
      err = err.substr(0, 67) + "…";
    
    return ftxui::hbox({
      ftxui::text("▸ ") | ftxui::color(ftxui::Color::Red),
      ftxui::text(summary + ": " + err) | ftxui::color(ftxui::Color::RedLight)
    });
  });
}

} // namespace firmius::tui

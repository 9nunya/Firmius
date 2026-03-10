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

static ftxui::Component SubagentWaitBlock(const std::shared_ptr<ToolCallView> &view) {
  auto opt = ftxui::ButtonOption::Simple();
  opt.transform = [](const ftxui::EntryState &s) {
    auto e = ftxui::text(s.label) | ftxui::dim;
    if (s.focused) e = e | ftxui::underlined;
    return e;
  };
  if (view) {
    opt.label = &view->toggle_label;
  } else {
    opt.label = "show";
  }
  opt.on_click = [view] {
    if (!view) return;
    view->show_result = !view->show_result;
  };

  auto toggle = ftxui::Button(opt);
  auto container = ftxui::Container::Horizontal({toggle});

  GlintConfig glint_cfg;
  glint_cfg.target = GlintConfig::Target::Text;
  glint_cfg.gradientColors = {
    ftxui::Color::Blue,
    ftxui::Color::White
  };
  glint_cfg.glintSize = 14;
  glint_cfg.intervalSeconds = 3;
  glint_cfg.durationSeconds = 1.2f;
  glint_cfg.easing = GlintEasing::EaseInOut;

  std::string parsed_agent_id;
  std::string parsed_title;
  if (view && !view->args.empty()) {
    rapidjson::Document doc;
    doc.Parse(view->args.c_str());
    if (!doc.HasParseError() && doc.IsObject()) {
      if (doc.HasMember("agent_id") && doc["agent_id"].IsString()) {
        parsed_agent_id = doc["agent_id"].GetString();
      }
      if (doc.HasMember("title") && doc["title"].IsString()) {
        parsed_title = doc["title"].GetString();
      }
    }
  }

  auto glint = GlintEffect(
    ftxui::text("Awaiting subagent " +
               (view->subagent_title.empty() ? parsed_title : view->subagent_title) +
               " (" +
               (!view->subagent_slug.empty() ? view->subagent_slug
                                              : (parsed_agent_id.empty() ?
                                                     view->agentId
                                                     : parsed_agent_id)) +
               ")"),
    glint_cfg
  );

  auto full_container = ftxui::Container::Vertical({container, glint});

  return ftxui::Renderer(full_container, [view, toggle, glint] {
    if (!view) return ftxui::text("[subagent_wait] <null>") | ftxui::dim;

    if (view->phase == ToolPhase::Preparing || view->phase == ToolPhase::Called) {
      return glint->Render();
    }

    if (view->success) {
      view->toggle_label = view->show_result ? "hide" : "show result";
      std::vector<ftxui::Element> rows;
      rows.push_back(ftxui::hbox({
        ftxui::text("[+] Subagent completed") | ftxui::bold,
        ftxui::text(" [") | ftxui::dim,
        toggle->Render(),
        ftxui::text("]") | ftxui::dim,
      }));

      if (view->show_result && !view->result.empty()) {
        std::string display = view->result;
        rapidjson::Document res;
        res.Parse(view->result.c_str());
        if (!res.HasParseError() && res.IsObject() && res.HasMember("result") && res["result"].IsString()) {
          display = res["result"].GetString();
        }
        if (display.size() > 200) display = display.substr(0, 197) + "...";

        std::vector<ftxui::Element> result_lines;
        result_lines.push_back(ftxui::text(display) | ftxui::dim);
        rows.push_back(LogWindow(result_lines, "await result"));
      }
      return ftxui::vbox(rows);
    }

    std::string err = view->result;
    if (err.empty()) err = "unknown error";
    return ftxui::text("[x] Await subagent failed: " + err) | ftxui::color(ftxui::Color::Red);
  });
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
    } else if (view->name == "summon_subagent") {
      return SubagentToolBlock(view);
    } else if (view->name == "subagent_wait") {
      return SubagentWaitBlock(view);
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
    std::string summary = SummarizeToolCall(view->name, view->args, view->phase);

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
        rows.push_back(LogWindow(out_lines, view->name));
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

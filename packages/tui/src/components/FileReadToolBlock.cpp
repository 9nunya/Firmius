#include "components/FileReadToolBlock.hpp"
#include "components/Markdown.hpp"
#include <ftxui/dom/elements.hpp>
#include <rapidjson/document.h>
#include <sstream>

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

ftxui::Component FileReadToolBlock(const std::shared_ptr<ToolCallView> &view) {
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
      return ftxui::text("[file_read] <null>") | ftxui::dim;

    std::string head = "[file_read] " + phaseLabel(view->phase);

    std::string path_arg = "";
    int start_line = -1;
    int end_line = -1;

    if (view->args.length() > 0) {
      rapidjson::Document doc;
      doc.Parse(view->args.c_str());
      if (!doc.HasParseError() && doc.IsObject()) {
        if (doc.HasMember("path") && doc["path"].IsString()) {
          path_arg = doc["path"].GetString();
        }
        if (doc.HasMember("start_line") && doc["start_line"].IsInt()) {
          start_line = doc["start_line"].GetInt();
        }
        if (doc.HasMember("end_line") && doc["end_line"].IsInt()) {
          end_line = doc["end_line"].GetInt();
        }
      }
    }

    std::string loc_str = path_arg;
    if (start_line != -1 && end_line != -1) {
      loc_str += " (" + std::to_string(start_line) + "-" +
                 std::to_string(end_line) + ")";
    }

    if (view->phase == ToolPhase::Preparing ||
        view->phase == ToolPhase::Called) {
      head = head + " " + loc_str;
    } else if (view->phase == ToolPhase::Finished) {
      if (view->success) {
        head = "Read " + loc_str + " ✓";
      } else {
        head = head + " " + loc_str + " ✗";
      }
    }

    bool can_toggle = (view->phase == ToolPhase::Finished && view->success &&
                       !view->result.empty());
    view->toggle_label = view->show_result ? "hide" : "show";

    std::vector<ftxui::Element> rows;
    if (can_toggle) {
      rows.push_back(ftxui::hbox(
          {ftxui::text(head) | ftxui::bold, ftxui::text(" [") | ftxui::dim,
           toggle->Render(), ftxui::text("]") | ftxui::dim}));

      if (view->show_result) {
        std::string content = view->result;
        rapidjson::Document res;
        res.Parse(view->result.c_str());
        if (!res.HasParseError() && res.IsObject() &&
            res.HasMember("content") && res["content"].IsString()) {
          content = res["content"].GetString();
        }

        std::stringstream ss(content);
        std::string line;
        int current_line = start_line != -1 ? start_line : 1;
        std::vector<ftxui::Element> code_lines;

        while (std::getline(ss, line)) {
          code_lines.push_back(ftxui::hbox(
              {ftxui::text(std::to_string(current_line) + " | ") | ftxui::dim,
               ftxui::text(line)}));
          current_line++;
        }
        rows.push_back(ftxui::vbox(std::move(code_lines)) | ftxui::border);
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

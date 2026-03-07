#include "components/FileEditToolBlock.hpp"
#include "components/Markdown.hpp"
#include <ftxui/dom/elements.hpp>
#include <rapidjson/document.h>

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

ftxui::Component FileEditToolBlock(const std::shared_ptr<ToolCallView> &view) {
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
      return ftxui::text("[file_edit] <null>") | ftxui::dim;

    std::string head = "[file_edit] " + phaseLabel(view->phase);

    std::string path_arg = "";
    std::string target_content = "";
    std::string replacement_content = "";

    if (view->args.length() > 0) {
      rapidjson::Document doc;
      doc.Parse(view->args.c_str());
      if (!doc.HasParseError() && doc.IsObject()) {
        if (doc.HasMember("path") && doc["path"].IsString()) {
          path_arg = doc["path"].GetString();
        }
        if (doc.HasMember("target_content") &&
            doc["target_content"].IsString()) {
          target_content = doc["target_content"].GetString();
        }
        if (doc.HasMember("replacement_content") &&
            doc["replacement_content"].IsString()) {
          replacement_content = doc["replacement_content"].GetString();
        }
      }
    }

    if (view->phase == ToolPhase::Preparing ||
        view->phase == ToolPhase::Called) {
      head = head + " " + path_arg;
    } else if (view->phase == ToolPhase::Finished) {
      if (view->success) {
        head = "Edited " + path_arg + " ✓";
      } else {
        head = head + " " + path_arg + " ✗";
      }
    }

    bool can_toggle = (view->phase == ToolPhase::Finished && view->success &&
                       !view->result.empty());
    view->toggle_label = view->show_result ? "hide" : "show";

    std::vector<ftxui::Element> rows;
    if (can_toggle || (view->phase == ToolPhase::Called)) {
      rows.push_back(ftxui::hbox(
          {ftxui::text(head) | ftxui::bold, ftxui::text(" [") | ftxui::dim,
           toggle->Render(), ftxui::text("]") | ftxui::dim}));

      if (view->show_result || view->phase == ToolPhase::Called) {
        if (!target_content.empty() || !replacement_content.empty()) {
          // Diff view
          std::vector<ftxui::Element> dict_rows;

          std::stringstream target_ss(target_content);
          std::string line;
          while (std::getline(target_ss, line)) {
            dict_rows.push_back(ftxui::hbox(
                {ftxui::text("- " + line) | ftxui::color(ftxui::Color::Red)}));
          }

          std::stringstream rep_ss(replacement_content);
          while (std::getline(rep_ss, line)) {
            dict_rows.push_back(
                ftxui::hbox({ftxui::text("+ " + line) |
                             ftxui::color(ftxui::Color::Green)}));
          }

          rows.push_back(ftxui::vbox(std::move(dict_rows)) | ftxui::border);
        } else if (view->phase == ToolPhase::Finished) {
          rows.push_back(firmius::tui::RenderMarkdown(view->result));
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

#include "components/ListDirectoryToolBlock.hpp"
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

ftxui::Component
ListDirectoryToolBlock(const std::shared_ptr<ToolCallView> &view) {
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
      return ftxui::text("[list_directory] <null>") | ftxui::dim;

    std::string head = "[list_directory] " + phaseLabel(view->phase);

    std::string path_arg = "";
    if (view->args.length() > 0) {
      rapidjson::Document doc;
      doc.Parse(view->args.c_str());
      if (!doc.HasParseError() && doc.IsObject() && doc.HasMember("path") &&
          doc["path"].IsString()) {
        path_arg = doc["path"].GetString();
      }
    }

    if (view->phase == ToolPhase::Preparing ||
        view->phase == ToolPhase::Called) {
      head = head + " " + path_arg;
    } else if (view->phase == ToolPhase::Finished) {
      if (view->success) {
        size_t num_items = 0;
        rapidjson::Document res;
        res.Parse(view->result.c_str());
        if (!res.HasParseError() && res.IsArray()) {
          num_items = res.Size();
        }
        head = "Listed " + path_arg + " (" + std::to_string(num_items) +
               " items) ✓";
      } else {
        head = head + " ✗";
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
        rapidjson::Document res;
        res.Parse(view->result.c_str());
        if (!res.HasParseError() && res.IsArray()) {
          for (rapidjson::SizeType i = 0; i < res.Size(); i++) {
            const auto &item = res[i];
            std::string prefix = "- ";
            if (item.HasMember("is_directory") &&
                item["is_directory"].GetBool()) {
              prefix = "d ";
            }
            if (item.HasMember("name") && item["name"].IsString()) {
              rows.push_back(ftxui::text(prefix + item["name"].GetString()) |
                             ftxui::dim);
            }
          }
        } else {
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

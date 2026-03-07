#include "components/ListDirectoryToolBlock.hpp"
#include <ftxui/dom/elements.hpp>
#include <rapidjson/document.h>

namespace firmius::tui {

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

    // Parse path from args
    std::string path_arg;
    if (!view->args.empty()) {
      rapidjson::Document doc;
      doc.Parse(view->args.c_str());
      if (!doc.HasParseError() && doc.IsObject() && doc.HasMember("path") &&
          doc["path"].IsString()) {
        path_arg = doc["path"].GetString();
      }
    }

    // ── Preparing / Called: one-liner ──
    if (view->phase == ToolPhase::Preparing ||
        view->phase == ToolPhase::Called) {
      return ftxui::text("[~] Listing " + path_arg + "...") | ftxui::dim;
    }

    // ── Finished ──
    if (view->success) {
      // Count items
      size_t num_items = 0;
      rapidjson::Document res;
      res.Parse(view->result.c_str());
      if (!res.HasParseError() && res.IsArray()) {
        num_items = res.Size();
      }

      view->toggle_label = view->show_result ? "hide" : "show";

      std::vector<ftxui::Element> rows;
      rows.push_back(
          ftxui::hbox({ftxui::text("[+] Listed " + path_arg + " (" +
                                   std::to_string(num_items) + " items)") |
                           ftxui::bold,
                       ftxui::text(" [") | ftxui::dim, toggle->Render(),
                       ftxui::text("]") | ftxui::dim}));

      if (view->show_result) {
        if (!res.HasParseError() && res.IsArray()) {
          for (rapidjson::SizeType i = 0; i < res.Size(); i++) {
            const auto &item = res[i];
            std::string prefix = "  ";
            if (item.HasMember("is_directory") &&
                item["is_directory"].GetBool()) {
              prefix = "d ";
            }
            if (item.HasMember("name") && item["name"].IsString()) {
              rows.push_back(ftxui::text(prefix + item["name"].GetString()) |
                             ftxui::dim);
            }
          }
        }
      }

      return ftxui::vbox(rows);
    }

    // Error state
    std::string err_msg = view->result;
    if (err_msg.empty())
      err_msg = "unknown error";
    return ftxui::text("[x] List " + path_arg + " failed: " + err_msg) |
           ftxui::color(ftxui::Color::Red);
  });
}

} // namespace firmius::tui

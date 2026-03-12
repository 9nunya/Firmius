#include "components/ListDirectoryToolBlock.hpp"
#include "utils/ErrorCleaner.hpp"
#include "utils/ToolView.hpp"
#include <ftxui/component/component.hpp>
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

      std::string path_display = path_arg;
      if (path_display.size() > 50) {
        path_display = "…" + path_display.substr(path_display.size() - 48);
      }

      std::vector<ftxui::Element> rows;
      rows.push_back(ftxui::hbox(
          {ftxui::text("▸ Listed " + path_display + " (" +
                       std::to_string(num_items) + " items)") |
               ftxui::bold | ftxui::color(ftxui::Color::RGB(100, 180, 220)) |
               ftxui::flex_shrink,
           ftxui::text("  [") | ftxui::dim, toggle->Render(),
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
              std::string name = item["name"].GetString();
              if (name.size() > 60) {
                name = name.substr(0, 58) + "…";
              }
              rows.push_back(ftxui::text(prefix + name) | ftxui::dim |
                             ftxui::flex_shrink);
            }
          }
        }
      }

      return ftxui::vbox(rows);
    }

    // Error state
    std::string err_msg = firmius::shared::ErrorCleaner::clean(view->result);
    return ftxui::vbox({
               ftxui::hbox(
                   {ftxui::text("[x] List ") | ftxui::color(ftxui::Color::Red),
                    ftxui::text(path_arg + " failed") | ftxui::bold |
                        ftxui::color(ftxui::Color::RedLight)}),
               ftxui::paragraph("  " + err_msg) |
                   ftxui::color(ftxui::Color::RedLight) | ftxui::flex_shrink,
           }) |
           ftxui::flex_shrink;
  });
}

} // namespace firmius::tui

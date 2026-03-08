#include "components/FileReadToolBlock.hpp"
#include "components/LogWindow.hpp"
#include <ftxui/dom/elements.hpp>
#include <rapidjson/document.h>
#include <sstream>

namespace firmius::tui {

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

    // Parse args
    std::string path_arg;
    int start_line = -1;
    int end_line = -1;
    if (!view->args.empty()) {
      rapidjson::Document doc;
      doc.Parse(view->args.c_str());
      if (!doc.HasParseError() && doc.IsObject()) {
        if (doc.HasMember("path") && doc["path"].IsString())
          path_arg = doc["path"].GetString();
        if (doc.HasMember("start_line") && doc["start_line"].IsInt())
          start_line = doc["start_line"].GetInt();
        if (doc.HasMember("end_line") && doc["end_line"].IsInt())
          end_line = doc["end_line"].GetInt();
      }
    }

    std::string loc_str = path_arg;
    if (start_line != -1 && end_line != -1) {
      loc_str += " (" + std::to_string(start_line) + "-" +
                 std::to_string(end_line) + ")";
    }

    // ── Preparing / Called ──
    if (view->phase == ToolPhase::Preparing ||
        view->phase == ToolPhase::Called) {
      return ftxui::text("[~] Reading " + loc_str + "...") | ftxui::dim;
    }

    // ── Finished + error ──
    if (!view->success) {
      std::string err_msg = view->result;
      if (err_msg.empty())
        err_msg = "unknown error";
      return ftxui::text("[x] Read " + loc_str + " failed: " + err_msg) |
             ftxui::color(ftxui::Color::Red);
    }

    // ── Finished + success: code window ──
    std::string content = view->result;
    {
      rapidjson::Document res;
      res.Parse(view->result.c_str());
      if (!res.HasParseError() && res.IsObject() && res.HasMember("content") &&
          res["content"].IsString()) {
        content = res["content"].GetString();
      }
    }

    // Split into lines
    std::vector<std::string> all_lines;
    {
      std::istringstream ss(content);
      std::string line;
      while (std::getline(ss, line)) {
        all_lines.push_back(line);
      }
    }

    int total_lines = static_cast<int>(all_lines.size());
    int preview_count = 5;
    bool has_more = total_lines > preview_count;
    int remaining = has_more ? total_lines - preview_count : 0;

    view->toggle_label =
        view->show_result ? "hide"
                          : ("show +" + std::to_string(remaining) + " lines");

    int lines_to_show =
        view->show_result ? total_lines : std::min(preview_count, total_lines);
    int line_num_start = (start_line != -1) ? start_line : 1;

    std::vector<ftxui::Element> code_lines;
    for (int i = 0; i < lines_to_show; i++) {
      int ln = line_num_start + i;
      std::string gutter = std::to_string(ln);
      // Pad gutter to consistent width
      while (gutter.size() < 3)
        gutter = " " + gutter;

      code_lines.push_back(ftxui::hbox({ftxui::text(gutter + "| ") | ftxui::dim,
                                        ftxui::text(all_lines[i])}));
    }

    if (has_more && !view->show_result) {
      code_lines.push_back(
          ftxui::text("   .. " + std::to_string(remaining) + " more lines..") |
          ftxui::dim);
    }

    // Footer
    std::string footer = "read " + path_arg;

    std::vector<ftxui::Element> rows;
    rows.push_back(LogWindow(code_lines, footer, view->toggle_label));
    if (has_more || view->show_result) {
      rows.push_back(
          ftxui::hbox({ftxui::text("[") | ftxui::dim, toggle->Render(),
                       ftxui::text("]") | ftxui::dim}));
    }

    return ftxui::vbox(rows);
  });
}

} // namespace firmius::tui

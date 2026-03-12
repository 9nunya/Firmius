#include "components/FileReadToolBlock.hpp"
#include "UIState.hpp"
#include "components/SyntaxHighlighter.hpp"
#include "utils/Hashline.hpp"
#include <algorithm>
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
      return ftxui::text("File read call") | ftxui::dim;

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

    // Get filename from path
    std::string filename = path_arg;
    auto pos = path_arg.find_last_of('/');
    if (pos != std::string::npos && pos + 1 < path_arg.size())
      filename = path_arg.substr(pos + 1);

    std::string loc_str = filename;
    if (loc_str.size() > 40) {
      loc_str = "…" + loc_str.substr(loc_str.size() - 38);
    }
    if (start_line != -1 && end_line != -1) {
      loc_str += " (" + std::to_string(start_line) + "-" +
                 std::to_string(end_line) + ")";
    }

    // ── Preparing / Called ──
    if (view->phase == ToolPhase::Preparing ||
        view->phase == ToolPhase::Called) {
      return ftxui::hbox({ftxui::text("⟳ ") | ftxui::color(ftxui::Color::Cyan),
                          ftxui::text("Reading ") | ftxui::dim,
                          ftxui::text(loc_str) |
                              ftxui::color(ftxui::Color::RGB(160, 180, 200))});
    }

    // ── Finished + error ──
    if (!view->success) {
      std::string err_msg = view->result;
      if (err_msg.empty())
        err_msg = "unknown error";
      if (err_msg.size() > 60)
        err_msg = err_msg.substr(0, 57) + "…";

      return ftxui::hbox({ftxui::text("▸ ") | ftxui::color(ftxui::Color::Red),
                          ftxui::text("Read failed: " + err_msg) |
                              ftxui::color(ftxui::Color::RedLight)}) |
             ftxui::borderRounded |
             ftxui::color(ftxui::Color::RGB(200, 100, 100));
    }

    // ── Finished + success: code window ──
    std::string content = view->result;
    rapidjson::Document res;
    res.Parse(view->result.c_str());
    int meta_line_start = start_line;
    int meta_line_end = end_line;
    bool read_full = false;
    if (!res.HasParseError() && res.IsObject()) {
      if (res.HasMember("content") && res["content"].IsString()) {
        content = res["content"].GetString();
      }
      if (res.HasMember("line_start") && res["line_start"].IsInt()) {
        meta_line_start = res["line_start"].GetInt();
      }
      if (res.HasMember("line_end") && res["line_end"].IsInt()) {
        meta_line_end = res["line_end"].GetInt();
      }
      if (res.HasMember("read_full") && res["read_full"].IsBool()) {
        read_full = res["read_full"].GetBool();
      }
    }

    // Trim hashline prefixes from content for clean display
    content = shared::utils::HashlineTrimmer::trimAll(content);

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
    int preview_count = 8;
    bool has_more = total_lines > preview_count;
    int remaining = has_more ? total_lines - preview_count : 0;

    view->toggle_label =
        view->show_result ? "hide"
                          : ("show +" + std::to_string(remaining) + " lines");

    int lines_to_show =
        view->show_result ? total_lines : std::min(preview_count, total_lines);
    int line_num_start = (meta_line_start != -1) ? meta_line_start : 1;

    // Use syntax highlighting if enabled
    bool use_syntax_highlight = UIState::instance().syntaxHighlightingEnabled;
    std::string detected_lang =
        SyntaxHighlighter::instance().detectLanguage(filename);

    ftxui::Elements code_lines;
    int gutter_width = std::to_string(line_num_start + lines_to_show).size();

    if (use_syntax_highlight &&
        SyntaxHighlighter::instance().hasGrammar(detected_lang)) {
      // Parse the snippet into a list of syntax-highlighted elements
      auto highlighted_lines =
          SyntaxHighlighter::instance().highlightRenderLines(content,
                                                             detected_lang);

      for (int i = 0; i < lines_to_show; i++) {
        int ln = line_num_start + i;
        std::string gutter = std::to_string(ln);
        while (static_cast<int>(gutter.size()) < gutter_width)
          gutter = " " + gutter;

        ftxui::Element lineElem;
        if (i < static_cast<int>(highlighted_lines.size())) {
          lineElem = highlighted_lines[i];
        } else {
          // Fallback if highlighted lines are missing
          std::string lineText =
              (i < static_cast<int>(all_lines.size())) ? all_lines[i] : "";
          lineElem = ftxui::text(lineText) |
                     ftxui::color(ftxui::Color::RGB(180, 180, 200));
        }

        code_lines.push_back(
            ftxui::hbox({ftxui::text(gutter + " │ ") | ftxui::dim |
                             ftxui::color(ftxui::Color::RGB(100, 100, 130)),
                         lineElem | ftxui::flex_shrink}) |
            ftxui::flex_shrink);
      }
    } else {
      // No syntax highlighting - use plain text
      for (int i = 0; i < lines_to_show; i++) {
        int ln = line_num_start + i;
        std::string gutter = std::to_string(ln);
        while (static_cast<int>(gutter.size()) < gutter_width)
          gutter = " " + gutter;

        code_lines.push_back(
            ftxui::hbox({ftxui::text(gutter + " │ ") | ftxui::dim |
                             ftxui::color(ftxui::Color::RGB(100, 100, 130)),
                         ftxui::text(all_lines[i]) |
                             ftxui::color(ftxui::Color::RGB(180, 180, 200))}));
      }
    }

    if (has_more && !view->show_result) {
      code_lines.push_back(
          ftxui::text("   … " + std::to_string(remaining) + " more lines") |
          ftxui::dim | ftxui::color(ftxui::Color::RGB(120, 120, 150)));
    }

    std::string footer;
    if (read_full) {
      footer = "Fully read " + filename;
    } else {
      int effective_end =
          meta_line_end > 0 ? meta_line_end : line_num_start + total_lines - 1;
      footer = filename + " (" + std::to_string(line_num_start) + "–" +
               std::to_string(effective_end) + ")";
    }

    ftxui::Elements rows;
    rows.push_back(ftxui::hbox(
        {ftxui::text("▸ ") | ftxui::color(ftxui::Color::RGB(100, 180, 220)),
         ftxui::text(filename + " ") | ftxui::bold |
             ftxui::color(ftxui::Color::RGB(140, 200, 240)),
         ftxui::filler(),
         ftxui::text(std::to_string(total_lines) + " lines") | ftxui::dim}));

    rows.push_back(ftxui::separatorLight());
    rows.push_back(ftxui::vbox(code_lines) | ftxui::frame | ftxui::flex_shrink);

    if (has_more || view->show_result) {
      rows.push_back(
          ftxui::hbox({ftxui::text("  [") | ftxui::dim, toggle->Render(),
                       ftxui::text("]") | ftxui::dim}));
    }

    return ftxui::vbox(rows) | ftxui::borderRounded |
           ftxui::color(ftxui::Color::RGB(130, 160, 180)) | ftxui::flex_shrink;
  });
}

} // namespace firmius::tui
